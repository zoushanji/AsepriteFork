// Universal Pixel Factory source backend.

#ifndef APP_CLI_PIXEL_JOB_H_INCLUDED
#define APP_CLI_PIXEL_JOB_H_INCLUDED
#pragma once

#include <string>

namespace app {

class Context;

int run_pixel_job(Context* context, const std::string& filename);

} // namespace app

#endif
