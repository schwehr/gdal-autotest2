// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// This is a complete rewrite of a file licensed as follows:
//
// Copyright (c) 2017, Even Rouault <even.rouault at spatialys.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

// See also:
//   https://github.com/OSGeo/proj.4/blob/master/test/fuzzers/standard_fuzzer.cpp

#include "third_party/proj4/proj_api.h"
#include "third_party/proj4/tests/proj4_fuzzer.pb.h"
#include "util/gtl/cleanup.h"

using third_party::proj4::tests::Proj4;

DEFINE_PROTO_FUZZER(const Proj4 &p) {
  projPJ src = pj_init_plus(p.src().c_str());
  if (src == nullptr) return;
  auto src_closer = gtl::MakeCleanup([src] { pj_free(src); });

  projPJ dst = pj_init_plus(p.dst().c_str());
  if (dst == nullptr) return;
  auto dst_closer = gtl::MakeCleanup([dst] { pj_free(dst); });

  double x = p.x();
  double y = p.y();
  pj_transform(src, dst, 1, 0, &x, &y, NULL);

  x = p.x();
  y = p.y();
  double z = p.z();
  pj_transform(src, dst, 1, 0, &x, &y, &z);

  pj_deallocate_grids();
}
