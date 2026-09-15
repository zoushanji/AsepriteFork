// Universal Pixel Factory source backend.
//
// This adapter intentionally accepts one versioned high-level job document.
// It is not a drawing-command RPC surface and does not execute Lua.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/cli/pixel_job.h"

#include "app/cmd/add_tag.h"
#include "app/cmd/remove_cel.h"
#include "app/cmd/set_layer_name.h"
#include "app/context.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/doc_api.h"
#include "app/file/file.h"
#include "app/transaction.h"
#include "base/exception.h"
#include "base/fs.h"
#include "doc/anidir.h"
#include "doc/cel.h"
#include "doc/layer.h"
#include "doc/sprite.h"
#include "doc/tag.h"
#include "json11.hpp"

#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace app {
namespace {

using json11::Json;

struct StateSpec {
  std::string name;
  int from = 0;
  int to = 0;
};

struct PixelJob {
  std::string operation;
  int width = 0;
  int height = 0;
  int frameCount = 0;
  int frameDurationMs = 0;
  std::vector<std::string> layers;
  std::vector<StateSpec> states;
  std::string output;
};

[[noreturn]] void invalid(const std::string& message)
{
  throw std::runtime_error("invalid pixel job: " + message);
}

const Json& required(const Json& object, const char* key)
{
  const Json& value = object[key];
  if (value.is_null())
    invalid(std::string("missing '") + key + "'");
  return value;
}

int required_int(const Json& object, const char* key, int low, int high)
{
  const Json& value = required(object, key);
  if (!value.is_number())
    invalid(std::string("'") + key + "' must be an integer");
  const int result = value.int_value();
  if (double(result) != value.number_value() || result < low || result > high)
    invalid(std::string("'") + key + "' is outside the supported range");
  return result;
}

std::string required_string(const Json& object, const char* key)
{
  const Json& value = required(object, key);
  if (!value.is_string() || value.string_value().empty())
    invalid(std::string("'") + key + "' must be a non-empty string");
  return value.string_value();
}

Json read_json(const std::string& filename)
{
  std::ifstream input(filename, std::ifstream::binary);
  if (!input)
    throw std::runtime_error("cannot open pixel job: " + filename);

  std::ostringstream contents;
  contents << input.rdbuf();
  std::string error;
  Json root = Json::parse(contents.str(), error);
  if (!error.empty())
    throw std::runtime_error("cannot parse pixel job: " + error);
  if (!root.is_object())
    invalid("root must be an object");
  return root;
}

PixelJob parse_job(const Json& root)
{
  PixelJob job;
  if (required_int(root, "schema_version", 1, 1) != 1)
    invalid("unsupported schema version");

  job.operation = required_string(root, "operation");
  if (job.operation == "probe")
    return job;
  if (job.operation != "initialize_document")
    invalid("unsupported operation '" + job.operation + "'");

  const Json& canvas = required(root, "canvas");
  if (!canvas.is_object())
    invalid("'canvas' must be an object");
  job.width = required_int(canvas, "width", 1, 4096);
  job.height = required_int(canvas, "height", 1, 4096);
  if (required_string(canvas, "color_mode") != "RGB")
    invalid("only RGB canvas mode is supported");

  const Json& timeline = required(root, "timeline");
  if (!timeline.is_object())
    invalid("'timeline' must be an object");
  job.frameCount = required_int(timeline, "frame_count", 1, 256);
  job.frameDurationMs = required_int(timeline, "frame_duration_ms", 1, 10000);

  const Json& states = required(timeline, "states");
  if (!states.is_array())
    invalid("'timeline.states' must be an array");
  std::vector<bool> covered(job.frameCount, false);
  std::set<std::string> stateNames;
  for (const Json& item : states.array_items()) {
    if (!item.is_object())
      invalid("each state must be an object");
    StateSpec state;
    state.name = required_string(item, "name");
    state.from = required_int(item, "from", 1, job.frameCount);
    state.to = required_int(item, "to", state.from, job.frameCount);
    if (!stateNames.insert(state.name).second)
      invalid("duplicate state name '" + state.name + "'");
    for (int frame = state.from; frame <= state.to; ++frame) {
      if (covered[frame - 1])
        invalid("state ranges overlap");
      covered[frame - 1] = true;
    }
    job.states.push_back(state);
  }
  for (bool frameCovered : covered) {
    if (!frameCovered)
      invalid("state ranges must cover every frame exactly once");
  }

  const Json& layers = required(root, "layers");
  if (!layers.is_array() || layers.array_items().empty())
    invalid("'layers' must be a non-empty array");
  std::set<std::string> layerNames;
  int expectedZ = 1;
  for (const Json& item : layers.array_items()) {
    if (!item.is_object())
      invalid("each layer must be an object");
    const std::string name = required_string(item, "name");
    const int z = required_int(item, "z_index", 1, int(layers.array_items().size()));
    if (z != expectedZ++)
      invalid("layers must use contiguous bottom-to-top z_index values");
    if (!layerNames.insert(name).second)
      invalid("duplicate layer name '" + name + "'");
    job.layers.push_back(name);
  }

  const Json& outputs = required(root, "outputs");
  if (!outputs.is_object())
    invalid("'outputs' must be an object");
  job.output = required_string(outputs, "aseprite");
  if (!base::is_absolute_path(job.output))
    invalid("output path must be absolute");
  if (base::get_file_extension(job.output) != "aseprite")
    invalid("output path must end in .aseprite");
  if (base::is_file(job.output))
    invalid("refusing to overwrite existing output");

  return job;
}

int initialize_document(Context* context, const PixelJob& job)
{
  Doc* document = context->documents().add(job.width, job.height, doc::ColorMode::RGB, 256);
  context->setActiveDocument(document);
  doc::Sprite* sprite = document->sprite();

  {
    ContextWriter writer(context);
    Transaction transaction(context, document, "Universal Pixel Factory: initialize document");
    DocApi api(document, transaction);

    doc::Layer* firstLayer = sprite->root()->firstLayer();
    transaction.execute(new cmd::SetLayerName(firstLayer, job.layers.front()));
    if (doc::Cel* defaultCel = firstLayer->cel(doc::frame_t(0)))
      transaction.execute(new cmd::RemoveCel(defaultCel));

    for (std::size_t i = 1; i < job.layers.size(); ++i)
      api.newLayer(sprite->root(), job.layers[i]);

    api.setTotalFrames(sprite, doc::frame_t(job.frameCount));
    for (doc::frame_t frame = 0; frame < sprite->totalFrames(); ++frame)
      api.setFrameDuration(sprite, frame, job.frameDurationMs);

    for (const StateSpec& state : job.states) {
      doc::Tag* tag = new doc::Tag(doc::frame_t(state.from - 1), doc::frame_t(state.to - 1));
      tag->setName(state.name);
      tag->setAniDir(doc::AniDir::FORWARD);
      transaction.execute(new cmd::AddTag(sprite, tag));
    }

    transaction.commit();
  }

  document->setFilename(job.output);
  if (save_document(context, document) != 0)
    throw std::runtime_error("Aseprite failed to save output: " + job.output);

  std::cout << "UPF_RESULT {\"ok\":true,\"operation\":\"initialize_document\","
            << "\"layers\":" << job.layers.size() << ",\"frames\":" << job.frameCount << "}"
            << std::endl;
  return 0;
}

} // anonymous namespace

int run_pixel_job(Context* context, const std::string& filename)
{
  const PixelJob job = parse_job(read_json(filename));
  if (job.operation == "probe") {
    std::cout << "UPF_RESULT {\"ok\":true,\"schema_version\":1,"
                 "\"backend\":\"aseprite-source\",\"lua\":false,"
                 "\"operations\":[\"probe\",\"initialize_document\"]}"
              << std::endl;
    return 0;
  }
  return initialize_document(context, job);
}

} // namespace app
