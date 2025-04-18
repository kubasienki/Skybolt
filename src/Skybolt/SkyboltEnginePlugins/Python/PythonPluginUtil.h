/* Copyright Matthew Reid
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <pybind11/pybind11.h>

namespace skybolt {

std::vector<pybind11::module> loadPythonPluginModules(const std::vector<std::string>& moduleNames);

} // namespace skybolt