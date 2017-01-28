#!/usr/bin/env python
# Copyright 2014 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

"""Test of coordinate transforms.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_ct.py

TODO(schwehr): What is the difference between CoordinateTransformation
  CreateCoordinateTransformation?
"""

import unittest


from osgeo import gdal
from osgeo import ogr
from osgeo import osr
import unittest
from autotest2.gcore import gcore_util
from autotest2.osr import osr_util

# Each tuple is:
# EPSCG Code, Longitude, Latitude, delta, projected u, projected v, delta_inv.
#
# TODO(schwehr): Uncomment the projections that did not work with GDAL 1.
TRANSFORM_POINTS = [
    # Anguilla 1957 - Anguilla 1957 / British West Indies Grid
    (2000, -65.592623, 0.0, 1e-4, 0.0, 0.0, 4.86146018375e-05),
    # Antigua 1943 - Antigua 1943 / British West Indies Grid
    (2001, -65.594764, 0.000642, 1e-4, 0.0, 0.0, 4.86159988213e-05),
    # Dominica 1945 - Dominica 1945 / British West Indies Grid
    (2002, -65.584149, 0.004848, 1e-4, 0.0, 0.0, 4.86187929598e-05),
    # Grenada 1953 - Grenada 1953 / British West Indies Grid
    (2003, -65.591241, 0.000841, 1e-4, 0.0, 0.0, 4.8615998864e-05),
    # Montserrat 1958 - Montserrat 1958 / British West Indies Grid
    (2004, -65.589867, 0.003301, 1e-4, 0.0, 0.0, 4.86159988213e-05),
    # St. Kitts 1955 - St. Kitts 1955 / British West Indies Grid
    (2005, -65.591870, 0.002134, 1e-4, 0.0, 0.0, 4.86159989494e-05),
    # St. Lucia 1955 - St. Lucia 1955 / British West Indies Grid
    (2006, -65.593367, 0.002677, 1e-4, 0.0, 0.0, 4.86153003294e-05),
    # St. Vincent 1945 - St. Vincent 45 / British West Indies Grid
    (2007, -65.589788, 0.002484, 1e-4, 0.0, 0.0, 4.86159989067e-05),
    # NAD27(CGQ77) - NAD27(CGQ77) / SCoPQ zone 2 (deprecated)
    (2008, -58.237260, 0.0, 1e-4, 0.0, 0.0, 1.09803804662e-05),
    # NAD27(CGQ77) - NAD27(CGQ77) / SCoPQ zone 3
    (2009, -61.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(CGQ77) - NAD27(CGQ77) / SCoPQ zone 4
    (2010, -64.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(CGQ77) - NAD27(CGQ77) / SCoPQ zone 5
    (2011, -67.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(CGQ77) - NAD27(CGQ77) / SCoPQ zone 6
    (2012, -70.237260, 0.0, 1e-4, 0.0, 0.0, 1.09836109914e-05),
    # NAD27(CGQ77) - NAD27(CGQ77) / SCoPQ zone 7
    (2013, -73.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(CGQ77) - NAD27(CGQ77) / SCoPQ zone 8
    (2014, -76.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(CGQ77) - NAD27(CGQ77) / SCoPQ zone 9
    (2015, -79.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(CGQ77) - NAD27(CGQ77) / SCoPQ zone 10
    (2016, -82.237260, 0.0, 1e-4, 0.0, 0.0, 1.09836109914e-05),
    # NAD27(76) - NAD27(76) / MTM zone 8
    (2017, -76.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(76) - NAD27(76) / MTM zone 9
    (2018, -79.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(76) - NAD27(76) / MTM zone 10
    (2019, -82.237260, 0.0, 1e-4, 0.0, 0.0, 1.09836109914e-05),
    # NAD27(76) - NAD27(76) / MTM zone 11
    (2020, -85.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(76) - NAD27(76) / MTM zone 12
    (2021, -83.737260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(76) - NAD27(76) / MTM zone 13
    (2022, -86.737260, 0.0, 1e-4, 0.0, 0.0, 1.09836109914e-05),
    # NAD27(76) - NAD27(76) / MTM zone 14
    (2023, -89.737260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(76) - NAD27(76) / MTM zone 15
    (2024, -92.737260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(76) - NAD27(76) / MTM zone 16
    (2025, -95.737260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27(76) - NAD27(76) / MTM zone 17
    (2026, -98.737260, 0.0, 1e-4, 0.0, 0.0, 1.09836109914e-05),
    # NAD27(76) - NAD27(76) / UTM zone 15N
    (2027, -97.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27(76) - NAD27(76) / UTM zone 16N
    (2028, -91.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170720682945),
    # NAD27(76) - NAD27(76) / UTM zone 17N
    (2029, -85.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27(76) - NAD27(76) / UTM zone 18N
    (2030, -79.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170720682945),
    # NAD27(CGQ77) - NAD27(CGQ77) / UTM zone 17N
    (2031, -85.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27(CGQ77) - NAD27(CGQ77) / UTM zone 18N
    (2032, -79.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170720682945),
    # NAD27(CGQ77) - NAD27(CGQ77) / UTM zone 19N
    (2033, -73.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27(CGQ77) - NAD27(CGQ77) / UTM zone 20N
    (2034, -67.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27(CGQ77) - NAD27(CGQ77) / UTM zone 21N
    (2035, -61.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD83(CSRS98) - NAD83(CSRS98) / New Brunswick Stereo (deprecated)
    (2036, -83.554088, -15.483645, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS98) - NAD83(CSRS98) / UTM zone 19N (deprecated)
    (2037, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS98) - NAD83(CSRS98) / UTM zone 20N (deprecated)
    (2038, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Israel 1993 - Israel 1993 / Israeli TM Grid
    (2039, 33.011727, 26.061987, 1e-4, 0.0, 0.0, 1.22188066598e-06),
    # Locodjo 1965 - Locodjo 1965 / UTM zone 30N
    (2040, -7.488339, 0.004223, 1e-4, 0.0, 0.0, 0.00017212840612),
    # Abidjan 1987 - Abidjan 1987 / UTM zone 30N
    (2041, -7.488339, 0.004222, 1e-4, 0.0, 0.0, 0.000172130152607),
    # Locodjo 1965 - Locodjo 1965 / UTM zone 29N
    (2042, -13.488464, 0.004223, 1e-4, 0.0, 0.0, 0.000172129017643),
    # Abidjan 1987 - Abidjan 1987 / UTM zone 29N
    (2043, -13.488463, 0.004222, 1e-4, 0.0, 0.0, 0.000172131811269),
    # Hanoi 1972 - Hanoi 1972 / Gauss-Kruger zone 18
    (2044, 156.971218, -0.000564, 1e-4, 0.0, 0.0, 37933969.7856),
    # Hanoi 1972 - Hanoi 1972 / Gauss-Kruger zone 19
    (2045, -141.015281, -0.000564, 1e-4, 0.0, 0.0, 4),
    # Hartebeesthoek94 - Hartebeesthoek94 / Lo15
    (2046, 15.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hartebeesthoek94 - Hartebeesthoek94 / Lo17
    (2047, 17.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hartebeesthoek94 - Hartebeesthoek94 / Lo19
    (2048, 19.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hartebeesthoek94 - Hartebeesthoek94 / Lo21
    (2049, 21.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hartebeesthoek94 - Hartebeesthoek94 / Lo23
    (2050, 23.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hartebeesthoek94 - Hartebeesthoek94 / Lo25
    (2051, 25.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hartebeesthoek94 - Hartebeesthoek94 / Lo27
    (2052, 27.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hartebeesthoek94 - Hartebeesthoek94 / Lo29
    (2053, 29.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hartebeesthoek94 - Hartebeesthoek94 / Lo31
    (2054, 31.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hartebeesthoek94 - Hartebeesthoek94 / Lo33
    (2055, 33.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # CH1903+ - CH1903+ / LV95
    (2056, -19.917996, 32.124486, 1e-4, 0.0, 0.0, 1e-07),
    # Rassadiran - Rassadiran / Nakhl e Taqi
    (2057, 46.709076, -0.008132, 1e-4, 0.0, 0.0, 2.28389399126e-06),
    # ED50(ED77) - ED50(ED77) / UTM zone 38N
    (2058, 40.511214, -0.001483, 1e-4, 0.0, 0.0, 0.000168835540578),
    # ED50(ED77) - ED50(ED77) / UTM zone 39N
    (2059, 46.511379, -0.001483, 1e-4, 0.0, 0.0, 0.000168837723365),
    # ED50(ED77) - ED50(ED77) / UTM zone 40N
    (2060, 52.511545, -0.001483, 1e-4, 0.0, 0.0, 0.000168835540535),
    # ED50(ED77) - ED50(ED77) / UTM zone 41N
    (2061, 58.511709, -0.001483, 1e-4, 0.0, 0.0, 0.000168836675627),
    # Madrid 1870 (Madrid) - Madrid 1870 (Madrid) / Spain
    (2062, -10.196179, 34.398226, 1e-4, 0.0, 0.0, 1e-07),
    # Conakry 1905 - Dabola 1981 / UTM zone 28N (deprecated)
    (2063, -19.486540, -0.000081, 1e-4, 0.0, 0.0, 0.000172129104612),
    # Conakry 1905 - Dabola 1981 / UTM zone 29N (deprecated)
    (2064, -13.486450, -0.000081, 1e-4, 0.0, 0.0, 0.000172128231497),
    # S-JTSK (Ferro) - S-JTSK (Ferro) / Krovak
    (2065, 24.830160, 59.755896, 1e-4, 0.0, 0.0, 1e-07),
    # Mount Dillon - Mount Dillon / Tobago Grid
    (2066, -61.031045, 10.924614, 1e-4, 0.0, 0.0, 2.7109847586e-06),
    # Naparima 1955 - Naparima 1955 / UTM zone 20N
    (2067, -67.487292, 0.001553, 1e-4, 0.0, 0.0, 0.000168836675584),
    # ELD79 - ELD79 / Libya zone 5
    (2068, 7.202805, -0.001379, 1e-4, 0.0, 0.0, 1.19734617955e-06),
    # ELD79 - ELD79 / Libya zone 6
    (2069, 9.202845, -0.001379, 1e-4, 0.0, 0.0, 1.19848118629e-06),
    # ELD79 - ELD79 / Libya zone 7
    (2070, 11.202886, -0.001379, 1e-4, 0.0, 0.0, 1.19682226796e-06),
    # ELD79 - ELD79 / Libya zone 8
    (2071, 13.202928, -0.001379, 1e-4, 0.0, 0.0, 1.19874316342e-06),
    # ELD79 - ELD79 / Libya zone 9
    (2072, 15.202971, -0.001379, 1e-4, 0.0, 0.0, 1.19935434386e-06),
    # ELD79 - ELD79 / Libya zone 10
    (2073, 17.203015, -0.001379, 1e-4, 0.0, 0.0, 1.19708424508e-06),
    # ELD79 - ELD79 / Libya zone 11
    (2074, 19.203059, -0.001379, 1e-4, 0.0, 0.0, 1.19516343492e-06),
    # ELD79 - ELD79 / Libya zone 12
    (2075, 21.203104, -0.001379, 1e-4, 0.0, 0.0, 1.19760811403e-06),
    # ELD79 - ELD79 / Libya zone 13
    (2076, 23.203149, -0.001379, 1e-4, 0.0, 0.0, 1.19708424508e-06),
    # ELD79 - ELD79 / UTM zone 32N
    (2077, 4.510627, -0.001379, 1e-4, 0.0, 0.0, 0.000168836675627),
    # ELD79 - ELD79 / UTM zone 33N
    (2078, 10.510747, -0.001379, 1e-4, 0.0, 0.0, 0.000168836675627),
    # ELD79 - ELD79 / UTM zone 34N
    (2079, 16.510875, -0.001379, 1e-4, 0.0, 0.0, 0.0001688352786),
    # ELD79 - ELD79 / UTM zone 35N
    (2080, 22.511009, -0.001379, 1e-4, 0.0, 0.0, 0.000168835278643),
    # Chos Malal 1914 - Chos Malal 1914 / Argentina 2
    (2081, -69.0, 90.0, 1e-4, 0.0, 0.0, 33756864.897),
    # Pampa del Castillo - Pampa del Castillo / Argentina 2
    (2082, 26.980231, 89.999724, 1e-4, 0.0, 0.0, 33756864.897),
    # Hito XVIII 1963 - Hito XVIII 1963 / Argentina 2
    (2083, 85.333142, 89.998239, 1e-4, 0.0, 0.0, 33756864.897),
    # Hito XVIII 1963 - Hito XVIII 1963 / UTM zone 19S
    (2084, 85.333142, -89.998239, 1e-4, 0.0, 0.0, 752569),
    # NAD27 - NAD27 / Cuba Norte (deprecated)
    (2085, -85.766571, 19.747527, 1e-4, 0.0, 0.0, 1.64145603776e-07),
    # NAD27 - NAD27 / Cuba Sur (deprecated)
    (2086, -81.567899, 18.581117, 1e-4, 0.0, 0.0, 1.67070538737e-07),
    # ELD79 - ELD79 / TM 12 NE
    (2087, 7.510686, -0.001379, 1e-4, 0.0, 0.0, 0.00016884060473),
    # Carthage - Carthage / TM 11 NE
    (2088, 6.511657, 0.003898, 1e-4, 0.0, 0.0, 0.000172128755366),
    # Yemen NGN96 - Yemen NGN96 / UTM zone 38N
    (2089, 40.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # Yemen NGN96 - Yemen NGN96 / UTM zone 39N
    (2090, 46.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # South Yemen - South Yemen / Gauss Kruger zone 8 (deprecated)
    (2091, -13.542860, 0.000606, 1e-4, 0.0, 0.0, 675426),
    # South Yemen - South Yemen / Gauss Kruger zone 9 (deprecated)
    (2092, -8.938666, 0.000606, 1e-4, 0.0, 0.0, 1736227),
    # Hanoi 1972 - Hanoi 1972 / GK 106 NE
    (2093, 101.513472, -0.000564, 1e-4, 0.0, 0.0, 0.000167353864537),
    # WGS 72BE - WGS 72BE / TM 106 NE
    (2094, 101.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316724705044),
    # Bissau - Bissau / UTM zone 28N
    (2095, -19.486943, 0.000244, 1e-4, 0.0, 0.0, 0.000168836675584),
    # Korean 1985 - Korean 1985 / East Belt
    (2096, 126.848244, 33.474487, 1e-4, 0.0, 0.0, 1.83284282684e-06),
    # Korean 1985 - Korean 1985 / Central Belt
    (2097, 124.848244, 33.474487, 1e-4, 0.0, 0.0, 1.83284282684e-06),
    # Korean 1985 - Korean 1985 / West Belt
    (2098, 122.848244, 33.474487, 1e-4, 0.0, 0.0, 1.83284282684e-06),
    # Qatar 1948 - Qatar 1948 / Qatar Grid
    (2099, 49.774941, 24.476394, 1e-4, 0.0, 0.0, 0.000354116069502),
    # GGRS87 - GGRS87 / Greek Grid
    (2100, 19.512489, 0.002230, 1e-4, 0.0, 0.0, 0.000167730613654),
    # Lake - Lake / Maracaibo Grid M1
    (2101, -71.605618, 10.642966, 1e-4, 0.0, 0.0, 1e-07),
    # Lake - Lake / Maracaibo Grid
    (2102, -73.423099, 8.829834, 1e-4, 0.0, 0.0, 1e-07),
    # Lake - Lake / Maracaibo Grid M3
    (2103, -76.111236, 6.094186, 1e-4, 0.0, 0.0, 1e-07),
    # Lake - Lake / Maracaibo La Rosa Grid
    (2104, -71.449988, 10.375831, 1e-4, 0.0, 0.0, 1e-07),
    # NZGD2000 - NZGD2000 / Mount Eden 2000
    (2105, 169.778770, -43.975883, 1e-4, 0.0, 0.0, 0.000231299403822),
    # NZGD2000 - NZGD2000 / Bay of Plenty 2000
    (2106, 171.406022, -44.852107, 1e-4, 0.0, 0.0, 0.000246877869358),
    # NZGD2000 - NZGD2000 / Poverty Bay 2000
    (2107, 172.748334, -45.710958, 1e-4, 0.0, 0.0, 0.000267677736701),
    # NZGD2000 - NZGD2000 / Hawkes Bay 2000
    (2108, 171.440012, -46.731876, 1e-4, 0.0, 0.0, 0.000330386334099),
    # NZGD2000 - NZGD2000 / Taranaki 2000
    (2109, 169.043218, -46.219364, 1e-4, 0.0, 0.0, 0.000297531281831),
    # NZGD2000 - NZGD2000 / Tuhirangi 2000
    (2110, 170.419725, -46.594013, 1e-4, 0.0, 0.0, 0.000321260275086),
    # NZGD2000 - NZGD2000 / Wanganui 2000
    (2111, 170.196516, -47.319762, 1e-4, 0.0, 0.0, 0.000371827511117),
    # NZGD2000 - NZGD2000 / Wairarapa 2000
    (2112, 170.286369, -47.999298, 1e-4, 0.0, 0.0, 0.00042531627696),
    # NZGD2000 - NZGD2000 / Wellington 2000
    (2113, 169.376305, -48.373009, 1e-4, 0.0, 0.0, 0.000457565824036),
    # NZGD2000 - NZGD2000 / Collingwood 2000
    (2114, 167.332722, -47.789921, 1e-4, 0.0, 0.0, 0.000408152583987),
    # NZGD2000 - NZGD2000 / Nelson 2000
    (2115, 167.901893, -48.346494, 1e-4, 0.0, 0.0, 0.000455201603472),
    # NZGD2000 - NZGD2000 / Karamea 2000
    (2116, 166.710006, -48.361685, 1e-4, 0.0, 0.0, 0.000456551788375),
    # NZGD2000 - NZGD2000 / Buller 2000
    (2117, 166.126549, -48.879539, 1e-4, 0.0, 0.0, 0.000504810508573),
    # NZGD2000 - NZGD2000 / Grey 2000
    (2118, 166.037617, -49.399553, 1e-4, 0.0, 0.0, 0.000557957711862),
    # NZGD2000 - NZGD2000 / Amuri 2000
    (2119, 167.457847, -49.752736, 1e-4, 0.0, 0.0, 0.000596996600507),
    # NZGD2000 - NZGD2000 / Marlborough 2000
    (2120, 168.376029, -48.614956, 1e-4, 0.0, 0.0, 0.000479601323605),
    # NZGD2000 - NZGD2000 / Hokitika 2000
    (2121, 165.404994, -49.948786, 1e-4, 0.0, 0.0, 0.000619764556177),
    # NZGD2000 - NZGD2000 / Okarito 2000
    (2122, 164.660174, -50.171334, 1e-4, 0.0, 0.0, 0.000646626547677),
    # NZGD2000 - NZGD2000 / Jacksons Bay 2000
    (2123, 162.901795, -51.033822, 1e-4, 0.0, 0.0, 0.000761780625908),
    # NZGD2000 - NZGD2000 / Mount Pleasant 2000
    (2124, 167.069512, -50.648979, 1e-4, 0.0, 0.0, 0.000708120118361),
    # NZGD2000 - NZGD2000 / Gawler 2000
    (2125, 165.684110, -50.806067, 1e-4, 0.0, 0.0, 0.00072955727228),
    # NZGD2000 - NZGD2000 / Timaru 2000
    (2126, 165.300348, -51.455347, 1e-4, 0.0, 0.0, 0.000825188762974),
    # NZGD2000 - NZGD2000 / Lindis Peak 2000
    (2127, 163.668451, -51.786302, 1e-4, 0.0, 0.0, 0.000878652819665),
    # NZGD2000 - NZGD2000 / Mount Nicholas 2000
    (2128, 162.548117, -52.181538, 1e-4, 0.0, 0.0, 0.000947116815951),
    # NZGD2000 - NZGD2000 / Mount York 2000
    (2129, 161.831035, -52.609578, 1e-4, 0.0, 0.0, 0.00102744530886),
    # NZGD2000 - NZGD2000 / Observation Point 2000
    (2130, 164.686627, -52.860422, 1e-4, 0.0, 0.0, 0.0010777671996),
    # NZGD2000 - NZGD2000 / North Taieri 2000
    (2131, 164.334344, -52.905675, 1e-4, 0.0, 0.0, 0.00108739189454),
    # NZGD2000 - NZGD2000 / Bluff 2000
    (2132, 162.291805, -53.639069, 1e-4, 0.0, 0.0, 0.0012510792003),
    # NZGD2000 - NZGD2000 / UTM zone 58S
    (2133, 165.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # NZGD2000 - NZGD2000 / UTM zone 59S
    (2134, 171.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # NZGD2000 - NZGD2000 / UTM zone 60S
    (2135, 177.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Accra - Accra / Ghana National Grid
    (2136, -3.471996, 4.665124, 1e-4, 0.0, 0.0, 1.97488222667e-05),
    # Accra - Accra / TM 1 NW
    (2137, -5.488514, 0.002912, 1e-4, 0.0, 0.0, 0.000169763050593),
    # NAD27(CGQ77) - NAD27(CGQ77) / Quebec Lambert
    (2138, -68.500000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS98) - NAD83(CSRS98) / SCoPQ zone 2 (deprecated)
    (2139, -58.237290, 0.0, 1e-4, 0.0, 0.0, 1.07595697045e-05),
    # NAD83(CSRS98) - NAD83(CSRS98) / MTM zone 3 (deprecated)
    (2140, -61.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS98) - NAD83(CSRS98) / MTM zone 4 (deprecated)
    (2141, -64.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS98) - NAD83(CSRS98) / MTM zone 5 (deprecated)
    (2142, -67.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS98) - NAD83(CSRS98) / MTM zone 6 (deprecated)
    (2143, -70.237290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83(CSRS98) - NAD83(CSRS98) / MTM zone 7 (deprecated)
    (2144, -73.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS98) - NAD83(CSRS98) / MTM zone 8 (deprecated)
    (2145, -76.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS98) - NAD83(CSRS98) / MTM zone 9 (deprecated)
    (2146, -79.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS98) - NAD83(CSRS98) / MTM zone 10 (deprecated)
    (2147, -82.237290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83(CSRS98) - NAD83(CSRS98) / UTM zone 21N (deprecated)
    (2148, -61.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS98) - NAD83(CSRS98) / UTM zone 18N (deprecated)
    (2149, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS98) - NAD83(CSRS98) / UTM zone 17N (deprecated)
    (2150, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS98) - NAD83(CSRS98) / UTM zone 13N (deprecated)
    (2151, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS98) - NAD83(CSRS98) / UTM zone 12N (deprecated)
    (2152, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS98) - NAD83(CSRS98) / UTM zone 11N (deprecated)
    (2153, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # RGF93 - RGF93 / Lambert-93
    (2154, -1.363081, -5.983856, 1e-4, 0.0, 0.0, 1e-07),
    # American Samoa 1962 / American Samoa Lambert (deprecated)
    (2155, 168.586861, -14.257425, 1e-4, 0.0, 0.0, 5.57053767553e-07),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 59S (deprecated)
    (2156, 171.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # IRENET95 - IRENET95 / Irish Transverse Mercator
    (2157, -15.817314, 46.488181, 1e-4, 0.0, 0.0, 0.00725392304594),
    # IRENET95 - IRENET95 / UTM zone 29N
    (2158, -13.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # Sierra Leone 1924 - Sierra Leone 1924 / New Colony Grid
    (2159, -13.378118, 6.664743, 1e-4, 0.0, 0.0, 8.91315210572e-07),
    # Sierra Leone 1924 - Sierra Leone 1924 / New War Office Grid
    (2160, -14.198200, 5.009194, 1e-4, 0.0, 0.0, 1.05319586608e-05),
    # Sierra Leone 1968 - Sierra Leone 1968 / UTM zone 28N
    (2161, -19.488894, 0.000913, 1e-4, 0.0, 0.0, 0.000172127882273),
    # Sierra Leone 1968 - Sierra Leone 1968 / UTM zone 29N
    (2162, -13.488814, 0.000913, 1e-4, 0.0, 0.0, 0.000172128755409),
    # Unspecified Clarke 1866 Authalic Sphere - US National Atlas Equal Area
    (2163, -100.0, 45.0, 1e-4, 0.0, 0.0, 1e-07),
    # Locodjo 1965 - Locodjo 1965 / TM 5 NW
    (2164, -9.488380, 0.004223, 1e-4, 0.0, 0.0, 0.000172128144528),
    # Abidjan 1987 - Abidjan 1987 / TM 5 NW
    (2165, -9.488380, 0.004222, 1e-4, 0.0, 0.0, 0.000172128144271),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / Gauss Kruger zone 3 (deprecated)
    (2166, -20.962281, -0.000705, 1e-4, 0.0, 0.0, 325),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / Gauss Kruger zone 4 (deprecated)
    (2167, -25.408587, -0.000705, 1e-4, 0.0, 0.0, 2788),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / Gauss Kruger zone 5 (deprecated)
    (2168, -29.144763, -0.000705, 1e-4, 0.0, 0.0, 15649),
    # Luxembourg 1930 - Luxembourg 1930 / Gauss
    (2169, 5.076318, 48.930106, 1e-4, 0.0, 0.0, 0.00487410723144),
    # MGI - MGI / Slovenia Grid (deprecated)
    (2170, 10.513402, 0.004052, 1e-4, 0.0, 0.0, 0.0131700418074),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Poland zone I (deprecated)
    (2171, -12.099239, -2.053591, 1e-4, 0.0, 0.0, 0.000262596644461),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Poland zone II
    (2172, -11.132365, -1.021091, 1e-4, 0.0, 0.0, 0.000264148693532),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Poland zone III
    (2173, -8.019222, 0.122988, 1e-4, 0.0, 0.0, 0.000257223146036),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Poland zone IV
    (2174, -10.308258, 0.616109, 1e-4, 0.0, 0.0, 0.000254635931924),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Poland zone V
    (2175, 16.078318, 42.398772, 1e-4, 0.0, 0.0, 0.00024444751034),
    # ETRS89 - ETRS89 / Poland CS2000 zone 5
    (2176, -29.147204, 0.0, 1e-4, 0.0, 0.0, 15660),
    # ETRS89 - ETRS89 / Poland CS2000 zone 6
    (2177, -32.071736, 0.0, 1e-4, 0.0, 0.0, 66269),
    # ETRS89 - ETRS89 / Poland CS2000 zone 7
    (2178, -33.999889, 0.0, 1e-4, 0.0, 0.0, 229060),
    # ETRS89 - ETRS89 / Poland CS2000 zone 8
    (2179, -34.543592, 0.0, 1e-4, 0.0, 0.0, 675909),
    # ETRS89 - ETRS89 / Poland CS92
    (2180, 12.335660, 47.673747, 1e-4, 0.0, 0.0, 0.00214617303573),
    # Azores Occidental 1939 - Azores Occidental 1939 / UTM zone 25N
    (2188, -37.492096, 0.000733, 1e-4, 0.0, 0.0, 0.000168835540556),
    # Azores Central 1948 - Azores Central 1948 / UTM zone 26N
    (2189, -31.487776, -0.000344, 1e-4, 0.0, 0.0, 0.000168835278611),
    # Azores Oriental 1940 - Azores Oriental 1940 / UTM zone 26N
    (2190, -31.488440, 0.000479, 1e-4, 0.0, 0.0, 0.000168834143551),
    # Madeira 1936 - Madeira 1936 / UTM zone 28N (deprecated)
    (2191, -19.488567, 0.0, 1e-4, 0.0, 0.0, 0.0001688352786),
    # ED50 - ED50 / France EuroLambert (deprecated)
    (2192, -3.404894, 27.140117, 1e-4, 0.0, 0.0, 1e-07),
    # NZGD2000 - NZGD2000 / New Zealand Transverse Mercator 2000
    (2193, 173.0, -90.0, 1e-4, 0.0, 0.0, 2403053),
    # American Samoa 1962 / American Samoa Lambert (deprecated)
    (2194, -171.413518, -14.257514, 1e-4, 0.0, 0.0, 5.43590408343e-07),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 2S
    (2195, -171.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # ETRS89 - ETRS89 / Kp2000 Jutland
    (2196, 7.703576, 0.0, 1e-4, 0.0, 0.0, 1.18590833154e-06),
    # ETRS89 - ETRS89 / Kp2000 Zealand
    (2197, 7.512824, 0.0, 1e-4, 0.0, 0.0, 0.000167446065461),
    # ETRS89 - ETRS89 / Kp2000 Bornholm
    (2198, 6.942039, 0.0, 1e-4, 0.0, 0.0, 0.00677224982064),
    # Albanian 1987 - Albanian 1987 / Gauss Kruger zone 4 (deprecated)
    (2199, -16.406844, 0.000567, 2e-3, 0.0, 0.0, 2788),
    # ATS77 - ATS77 / New Brunswick Stereographic (ATS77)
    (2200, -69.962391, 39.252016, 1e-4, 0.0, 0.0, 1e-07),
    # REGVEN - REGVEN / UTM zone 18N
    (2201, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # REGVEN - REGVEN / UTM zone 19N
    (2202, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # REGVEN - REGVEN / UTM zone 20N
    (2203, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD27 - NAD27 / Tennessee
    (2204, -92.617116, 34.206150, 1e-3, 0.0, 0.0, 2.39135756601e-07),
    # NAD83 - NAD83 / Kentucky North
    (2205, -89.896811, 37.361875, 1e-4, 0.0, 0.0, 1e-07),
    # ED50 - ED50 / 3-degree Gauss-Kruger zone 9
    (2206, -32.938202, -0.001094, 1e-4, 0.0, 0.0, 1735946),
    # ED50 - ED50 / 3-degree Gauss-Kruger zone 10
    (2207, -27.809357, -0.001094, 1e-4, 0.0, 0.0, 3900057),
    # ED50 - ED50 / 3-degree Gauss-Kruger zone 11
    (2208, -16.862779, -0.001094, 1e-4, 0.0, 0.0, 7619207),
    # ED50 - ED50 / 3-degree Gauss-Kruger zone 12
    (2209, 3.491115, -0.001094, 1e-4, 0.0, 0.0, 13002327.3465),
    # ED50 - ED50 / 3-degree Gauss-Kruger zone 13
    (2210, 38.605646, -0.001094, 1e-4, 0.0, 0.0, 20184181.324),
    # ED50 - ED50 / 3-degree Gauss-Kruger zone 14
    (2211, 96.157790, -0.001094, 1e-4, 0.0, 0.0, 32533707.9982),
    # ED50 - ED50 / 3-degree Gauss-Kruger zone 15
    (2212, -173.191353, -0.001094, 1e-4, 0.0, 0.0, 5),
    # ETRS89 - ETRS89 / TM 30 NE
    (2213, 25.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # Douala 1948 - Douala 1948 / AOF west (deprecated)
    (2214, 1.433278, -8.940758, 1e-4, 0.0, 0.0, 0.0126349318307),
    # Manoca 1962 - Manoca 1962 / UTM zone 32N
    (2215, 4.510026, -0.000374, 1e-4, 0.0, 0.0, 0.000172129890427),
    # Qornoq 1927 - Qornoq 1927 / UTM zone 22N
    (2216, -55.486651, -0.001709, 1e-4, 0.0, 0.0, 0.000168835278643),
    # Qornoq 1927 - Qornoq 1927 / UTM zone 23N
    (2217, -49.486642, -0.001709, 1e-4, 0.0, 0.0, 0.000168836675627),
    # ATS77 - ATS77 / UTM zone 19N
    (2219, -73.488745, 0.0, 1e-4, 0.0, 0.0, 0.000167729565874),
    # ATS77 - ATS77 / UTM zone 20N
    (2220, -67.488745, 0.0, 1e-4, 0.0, 0.0, 0.000167731748661),
    # NAD83 - NAD83 / Arizona East (ft)
    (2222, -112.400208, 30.980684, 1e-4, 0.0, 0.0, 6.68665136447e-06),
    # NAD83 - NAD83 / Arizona Central (ft)
    (2223, -114.150208, 30.980684, 1e-4, 0.0, 0.0, 6.68061167961e-06),
    # NAD83 - NAD83 / Arizona West (ft)
    (2224, -115.983467, 30.980685, 1e-4, 0.0, 0.0, 6.68785825863e-06),
    # NAD83 - NAD83 / California zone 1 (ftUS)
    (2225, -143.321156, 32.649532, 1e-4, 0.0, 0.0, 2.79865998891e-07),
    # NAD83 - NAD83 / California zone 2 (ftUS)
    (2226, -142.954019, 31.095993, 1e-4, 0.0, 0.0, 3.30854891217e-07),
    # NAD83 - NAD83 / California zone 3 (ftUS)
    (2227, -141.218751, 30.009900, 1e-4, 0.0, 0.0, 3.35438162438e-07),
    # NAD83 - NAD83 / California zone 4 (ftUS)
    (2228, -139.486472, 28.915572, 1e-4, 0.0, 0.0, 3.79552147933e-07),
    # NAD83 - NAD83 / California zone 5 (ftUS)
    (2229, -138.152186, 27.196010, 1e-4, 0.0, 0.0, 4.19082862209e-07),
    # NAD83 - NAD83 / California zone 6 (ftUS)
    (2230, -136.178771, 25.945490, 1e-4, 0.0, 0.0, 4.50306397397e-07),
    # NAD83 - NAD83 / Colorado North (ftUS)
    (2231, -115.653598, 36.118325, 1e-4, 0.0, 0.0, 1.83330848813e-07),
    # NAD83 - NAD83 / Colorado Central (ftUS)
    (2232, -115.464063, 34.638233, 1e-4, 0.0, 0.0, 1.92210936802e-07),
    # NAD83 - NAD83 / Colorado South (ftUS)
    (2233, -115.330280, 33.489028, 1e-4, 0.0, 0.0, 2.39189466811e-07),
    # NAD83 - NAD83 / Connecticut (ftUS)
    (2234, -76.287472, 39.405061, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Delaware (ftUS)
    (2235, -77.692913, 37.977968, 1e-4, 0.0, 0.0, 9.25010817428e-06),
    # NAD83 - NAD83 / Florida East (ftUS)
    (2236, -82.970337, 24.320543, 1e-4, 0.0, 0.0, 1.6175350876e-06),
    # NAD83 - NAD83 / Florida West (ftUS)
    (2237, -83.970337, 24.320543, 1e-4, 0.0, 0.0, 1.61132241679e-06),
    # NAD83 - NAD83 / Florida North (ftUS)
    (2238, -90.650782, 28.853971, 1e-4, 0.0, 0.0, 3.25091364585e-07),
    # NAD83 - NAD83 / Georgia East (ftUS)
    (2239, -84.239141, 29.983687, 1e-4, 0.0, 0.0, 4.12373395515e-06),
    # NAD83 - NAD83 / Georgia West (ftUS)
    (2240, -91.398144, 29.801257, 1e-4, 0.0, 0.0, 0.00466783311378),
    # NAD83 - NAD83 / Idaho East (ftUS)
    (2241, -114.567265, 41.641591, 1e-4, 0.0, 0.0, 1.2961154679e-05),
    # NAD83 - NAD83 / Idaho Central (ftUS)
    (2242, -119.988244, 41.510438, 1e-4, 0.0, 0.0, 0.00183911055059),
    # NAD83 - NAD83 / Idaho West (ftUS)
    (2243, -125.292410, 41.269025, 1e-4, 0.0, 0.0, 0.0931805209225),
    # NAD83 - NAD83 / Indiana East (ftUS) (deprecated)
    (2244, -86.765434, 35.247718, 1e-4, 0.0, 0.0, 1.52608608914e-07),
    # NAD83 - NAD83 / Indiana West (ftUS) (deprecated)
    (2245, -96.908239, 34.852888, 1e-4, 0.0, 0.0, 0.0637416545774),
    # NAD83 - NAD83 / Kentucky North (ftUS)
    (2246, -89.896811, 37.361875, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Kentucky South (ftUS)
    (2247, -91.003100, 31.708902, 1e-4, 0.0, 0.0, 2.81011816696e-07),
    # NAD83 - NAD83 / Maryland (ftUS)
    (2248, -81.529189, 37.577262, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Massachusetts Mainland (ftUS)
    (2249, -73.651391, 34.244387, 1e-4, 0.0, 0.0, 2.03525887628e-07),
    # NAD83 - NAD83 / Massachusetts Island (ftUS)
    (2250, -76.433406, 40.845825, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Michigan North (ft)
    (2251, -158.790109, 11.663980, 1e-4, 0.0, 0.0, 7.44651609622e-07),
    # NAD83 - NAD83 / Michigan Central (ft)
    (2252, -144.322231, 22.874000, 1e-4, 0.0, 0.0, 6.80864563111e-07),
    # NAD83 - NAD83 / Michigan South (ft)
    (2253, -127.914179, 32.026364, 1e-4, 0.0, 0.0, 3.67591261454e-07),
    # NAD83 - NAD83 / Mississippi East (ftUS)
    (2254, -91.925475, 29.464053, 1e-4, 0.0, 0.0, 4.01310551576e-05),
    # NAD83 - NAD83 / Mississippi West (ftUS)
    (2255, -97.529032, 29.305235, 1e-4, 0.0, 0.0, 0.00454319441734),
    # NAD83 - NAD83 / Montana (ft)
    (2256, -116.985465, 43.991943, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / New Mexico East (ftUS)
    (2257, -106.060830, 30.988445, 1e-4, 0.0, 0.0, 1.5747154993e-06),
    # NAD83 - NAD83 / New Mexico Central (ftUS)
    (2258, -111.476675, 30.894188, 1e-4, 0.0, 0.0, 0.000900648487301),
    # NAD83 - NAD83 / New Mexico West (ftUS)
    (2259, -116.482847, 30.710010, 1e-4, 0.0, 0.0, 0.00791171227234),
    # NAD83 - NAD83 / New York East (ftUS)
    (2260, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 1.97644674377e-06),
    # NAD83 - NAD83 / New York Central (ftUS)
    (2261, -79.509326, 39.963054, 1e-4, 0.0, 0.0, 3.96810172177e-05),
    # NAD83 - NAD83 / New York West (ftUS)
    (2262, -82.677307, 39.927648, 1e-4, 0.0, 0.0, 0.000266040889871),
    # NAD83 - NAD83 / New York Long Island (ftUS)
    (2263, -77.519584, 40.112385, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / North Carolina (ftUS)
    (2264, -85.568291, 33.568155, 1e-4, 0.0, 0.0, 2.26943686538e-07),
    # NAD83 - NAD83 / North Dakota North (ft)
    (2265, -108.360607, 46.724303, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / North Dakota South (ft)
    (2266, -108.173906, 45.402819, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Oklahoma North (ftUS)
    (2267, -104.561592, 34.817203, 1e-4, 0.0, 0.0, 1.91522645799e-07),
    # NAD83 - NAD83 / Oklahoma South (ftUS)
    (2268, -104.434828, 33.160876, 1e-4, 0.0, 0.0, 2.41455848176e-07),
    # NAD83 - NAD83 / Oregon North (ft)
    (2269, -150.091081, 39.506127, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Oregon South (ft)
    (2270, -138.236892, 40.231913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Pennsylvania North (ftUS)
    (2271, -84.776606, 39.947400, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Pennsylvania South (ftUS)
    (2272, -84.693691, 39.120795, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / South Carolina (ft)
    (2273, -87.429394, 31.662328, 1e-4, 0.0, 0.0, 2.6761616217e-07),
    # NAD83 - NAD83 / Tennessee (ftUS)
    (2274, -92.508665, 34.153466, 1e-4, 0.0, 0.0, 1.98492272166e-07),
    # NAD83 - NAD83 / Texas North (ftUS)
    (2275, -103.450598, 25.015254, 1e-4, 0.0, 0.0, 4.1378345486e-07),
    # NAD83 - NAD83 / Texas North Central (ftUS)
    (2276, -103.763988, 13.819848, 1e-4, 0.0, 0.0, 5.39107277291e-07),
    # NAD83 - NAD83 / Texas Central (ftUS)
    (2277, -105.983204, 3.442337, 1e-4, 0.0, 0.0, 1.89059937838e-07),
    # NAD83 - NAD83 / Texas South Central (ftUS)
    (2278, -103.518030, -6.145758, 1e-4, 0.0, 0.0, 3.59786790796e-07),
    # NAD83 - NAD83 / Texas South (ftUS)
    (2279, -100.642123, -15.495006, 1e-4, 0.0, 0.0, 6.52829694445e-07),
    # NAD83 - NAD83 / Utah North (ft)
    (2280, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 2.76429098334e-07),
    # NAD83 - NAD83 / Utah Central (ft)
    (2281, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 5.19629413863e-07),
    # NAD83 - NAD83 / Utah South (ft)
    (2282, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 4.84108990865e-07),
    # NAD83 - NAD83 / Virginia North (ftUS)
    (2283, -109.123093, 14.944862, 1e-4, 0.0, 0.0, 6.84053229634e-07),
    # NAD83 - NAD83 / Virginia South (ftUS)
    (2284, -111.898764, 21.848820, 1e-4, 0.0, 0.0, 6.19314523647e-07),
    # NAD83 - NAD83 / Washington North (ftUS)
    (2285, -127.390662, 46.808297, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Washington South (ftUS)
    (2286, -126.863697, 45.151783, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Wisconsin North (ftUS)
    (2287, -97.607760, 44.907938, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Wisconsin Central (ftUS)
    (2288, -97.438214, 43.585869, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Wisconsin South (ftUS)
    (2289, -97.222171, 41.765988, 1e-4, 0.0, 0.0, 1e-07),
    # ATS77 - ATS77 / Prince Edward Isl. Stereographic (ATS77)
    (2290, -71.627792, 43.314226, 1e-4, 0.0, 0.0, 1e-07),
    # ATS77 - NAD83(CSRS98) / Prince Edward Isl. Stereographic (deprecated)
    (2291, -67.663925, 39.957437, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS98) / Prince Edward Isl. Stereographic (deprecated)
    (2292, -67.663924, 39.957439, 1e-4, 0.0, 0.0, 1e-07),
    # ATS77 - ATS77 / MTM Nova Scotia zone 4
    (2294, -98.911452, 0.0, 1e-4, 0.0, 0.0, 2790),
    # ATS77 - ATS77 / MTM Nova Scotia zone 5
    (2295, -108.648019, 0.0, 1e-4, 0.0, 0.0, 15663),
    # Batavia - Batavia / TM 109 SE
    (2308, 118.968811, -89.993030, 1e-4, 0.0, 0.0, 3),
    # WGS 84 - WGS 84 / TM 116 SE
    (2309, 116.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / TM 132 SE
    (2310, 132.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / TM 6 NE
    (2311, 1.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728518136),
    # Garoua - Garoua / UTM zone 33N
    (2312, 10.511335, 0.0, 1e-4, 0.0, 0.0, 0.000172126747202),
    # Kousseri - Kousseri / UTM zone 33N
    (2313, 10.511335, 0.0, 1e-4, 0.0, 0.0, 0.000172126747202),
    # Trinidad 1903 - Trinidad 1903 / Trinidad Grid (ftCla)
    (2314, -62.121202, 9.853380, 1e-4, 0.0, 0.0, 6.90122177682e-05),
    # Campo Inchauspe - Campo Inchauspe / UTM zone 19S
    (2315, 137.419509, -89.998200, 1e-4, 0.0, 0.0, 752569),
    # Campo Inchauspe - Campo Inchauspe / UTM zone 20S
    (2316, 137.419509, -89.998200, 1e-4, 0.0, 0.0, 752569),
    # PSAD56 - PSAD56 / ICN Regional
    (2317, -74.898693, -3.094620, 1e-4, 0.0, 0.0, 1e-07),
    # Ain el Abd - Ain el Abd / Aramco Lambert
    (2318, 47.999488, 25.089973, 1e-4, 0.0, 0.0, 1e-07),
    # ED50 - ED50 / TM27
    (2319, 22.512710, -0.001094, 1e-4, 0.0, 0.0, 0.0001685114403),
    # ED50 - ED50 / TM30
    (2320, 25.512766, -0.001094, 1e-4, 0.0, 0.0, 0.000168512313394),
    # ED50 - ED50 / TM33
    (2321, 28.512824, -0.001094, 1e-4, 0.0, 0.0, 0.00016851091641),
    # ED50 - ED50 / TM36
    (2322, 31.512882, -0.001094, 1e-4, 0.0, 0.0, 0.00016851091641),
    # ED50 - ED50 / TM39
    (2323, 34.512942, -0.001094, 1e-4, 0.0, 0.0, 0.00016851091641),
    # ED50 - ED50 / TM42
    (2324, 37.513002, -0.001094, 1e-4, 0.0, 0.0, 0.000168511178366),
    # ED50 - ED50 / TM45
    (2325, 40.513063, -0.001094, 1e-4, 0.0, 0.0, 0.000168511178387),
    # Hong Kong 1980 - Hong Kong 1980 Grid System
    (2326, 106.429946, 14.779944, 1e-4, 0.0, 0.0, 0.00297547731316),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 13
    (2327, 74.628698, 0.0, 1e-4, 0.0, 0.0, 20187999.8383),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 14
    (2328, 135.197618, 0.0, 1e-4, 0.0, 0.0, 32544645.418),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 15
    (2329, -131.123611, 0.0, 1e-4, 0.0, 0.0, 4),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 16
    (2330, 10.089499, 0.0, 1e-4, 0.0, 0.0, 1925895),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 17
    (2331, -142.089915, 0.0, 1e-4, 0.0, 0.0, 4),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 18
    (2332, 157.076114, 0.0, 1e-4, 0.0, 0.0, 37962203.2395),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 19
    (2333, -140.859135, 0.0, 1e-4, 0.0, 0.0, 4),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 20
    (2334, 83.765857, 0.0, 1e-4, 0.0, 0.0, 24858075.8735),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 21
    (2335, 160.164347, 0.0, 1e-4, 0.0, 0.0, 38945995.6551),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 22
    (2336, 148.703556, 0.0, 1e-4, 0.0, 0.0, 37107391.1492),
    # Xian 1980 - Xian 1980 / Gauss-Kruger zone 23
    (2337, 122.673745, 0.0, 1e-4, 0.0, 0.0, 33175597.114),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 75E
    (2338, 70.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 81E
    (2339, 76.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 87E
    (2340, 82.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 93E
    (2341, 88.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 99E
    (2342, 94.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 105E
    (2343, 100.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 111E
    (2344, 106.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 117E
    (2345, 112.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 123E
    (2346, 118.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 129E
    (2347, 124.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / Gauss-Kruger CM 135E
    (2348, 130.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 25
    (2349, -35.469065, 0.0, 1e-4, 0.0, 0.0, 0.92857795882),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 26
    (2350, 131.034201, 0.0, 1e-4, 0.0, 0.0, 50222059.8214),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 27
    (2351, 27.037381, 0.0, 1e-4, 0.0, 0.0, 30521153.9844),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 28
    (2352, -176.608042, 0.0, 1e-4, 0.0, 0.0, 5),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 29
    (2353, 78.446774, 0.0, 1e-4, 0.0, 0.0, 42816417.436),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 30
    (2354, -58.786979, 0.0, 1e-4, 0.0, 0.0, 2),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 31
    (2355, 34.761211, 0.0, 1e-4, 0.0, 0.0, 35268961.8112),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 32
    (2356, -60.141746, 0.0, 1e-4, 0.0, 0.0, 2),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 33
    (2357, -1.147208, 0.0, 1e-4, 0.0, 0.0, 0.0300338480157),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 34
    (2358, -120.183079, 0.0, 1e-4, 0.0, 0.0, 4),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 35
    (2359, 20.952451, 0.0, 1e-4, 0.0, 0.0, 29697611.4919),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 36
    (2360, -164.730166, 0.0, 1e-4, 0.0, 0.0, 80526733.018),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 37
    (2361, -124.452102, 0.0, 1e-4, 0.0, 0.0, 4),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 38
    (2362, 39.582536, 0.0, 1e-4, 0.0, 0.0, 39583259.6011),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 39
    (2363, -64.267513, 0.0, 1e-4, 0.0, 0.0, 2),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 40
    (2364, -31.229664, 0.0, 1e-4, 0.0, 0.0, 0.817590684247),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 41
    (2365, -93.953107, 0.0, 1e-4, 0.0, 0.0, 3),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 42
    (2366, -36.022931, 0.0, 1e-4, 0.0, 0.0, 0.943078125824),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 43
    (2367, 94.855412, 0.0, 1e-4, 0.0, 0.0, 59175200.0919),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 44
    (2368, -5.990586, 0.0, 1e-4, 0.0, 0.0, 0.156833165149),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger zone 45
    (2369, -172.701241, 0.0, 1e-4, 0.0, 0.0, 78522136.4078),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 75E
    (2370, 70.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 78E
    (2371, 73.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 81E
    (2372, 76.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 84E
    (2373, 79.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 87E
    (2374, 82.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 90E
    (2375, 85.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 93E
    (2376, 88.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 96E
    (2377, 91.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 99E
    (2378, 94.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 102E
    (2379, 97.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 105E
    (2380, 100.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 108E
    (2381, 103.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 111E
    (2382, 106.513050, 0.0, 1e-4, 0.0, 0.0, 0.000167406513356),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 114E
    (2383, 109.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 117E
    (2384, 112.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 120E
    (2385, 115.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 123E
    (2386, 118.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 126E
    (2387, 121.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 129E
    (2388, 124.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 132E
    (2389, 127.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # Xian 1980 - Xian 1980 / 3-degree Gauss-Kruger CM 135E
    (2390, 130.513050, 0.0, 1e-4, 0.0, 0.0, 0.00016740441788),
    # KKJ - KKJ / Finland zone 1
    (2391, 7.648134, -0.001018, 1e-4, 0.0, 0.0, 0.293724879778),
    # KKJ - KKJ / Finland zone 2
    (2392, 2.099514, -0.001148, 1e-4, 0.0, 0.0, 19),
    # KKJ - KKJ / Finland Uniform Coordinate System
    (2393, -2.961863, -0.001267, 1e-4, 0.0, 0.0, 325),
    # KKJ - KKJ / Finland zone 4
    (2394, -7.408131, -0.001370, 1e-4, 0.0, 0.0, 2788),
    # South Yemen - South Yemen / Gauss-Kruger zone 8
    (2395, -13.542860, 0.000606, 1e-4, 0.0, 0.0, 675426),
    # South Yemen - South Yemen / Gauss-Kruger zone 9
    (2396, -8.938666, 0.000606, 1e-4, 0.0, 0.0, 1736227),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 3
    (2397, -20.962281, -0.000705, 1e-4, 0.0, 0.0, 325),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 4
    (2398, -25.408587, -0.000705, 1e-4, 0.0, 0.0, 2788),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 5
    (2399, -29.144763, -0.000705, 1e-4, 0.0, 0.0, 15649),
    # RT90 - RT90 2.5 gon W (deprecated)
    (2400, 2.453631, 0.004846, 1e-4, 0.0, 0.0, 0.304336469826),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 25
    (2401, -36.596759, -0.000744, 1e-4, 0.0, 0.0, 0.958073964361),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 26
    (2402, 129.545039, -0.000744, 1e-4, 0.0, 0.0, 49820287.8768),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 27
    (2403, 25.087264, -0.000744, 1e-4, 0.0, 0.0, 29965077.9258),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 28
    (2404, -179.129902, -0.000744, 1e-4, 0.0, 0.0, 5),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 29
    (2405, 75.209821, -0.000744, 1e-4, 0.0, 0.0, 42267195.8383),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 30
    (2406, -62.902664, -0.000744, 1e-4, 0.0, 0.0, 2),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 31
    (2407, 29.569842, -0.000744, 1e-4, 0.0, 0.0, 33569247.8905),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 32
    (2408, -66.638426, -0.000744, 1e-4, 0.0, 0.0, 2),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 33
    (2409, -9.222119, -0.000744, 1e-4, 0.0, 0.0, 0.241399272387),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 34
    (2410, -130.149256, -0.000744, 1e-4, 0.0, 0.0, 4),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 35
    (2411, 8.722972, -0.000744, 1e-4, 0.0, 0.0, 0.228403314515),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 36
    (2412, -179.642394, -0.000744, 1e-4, 0.0, 0.0, 71962992.2841),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 37
    (2413, -142.538925, -0.000744, 1e-4, 0.0, 0.0, 4),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 38
    (2414, 17.758132, -0.000744, 1e-4, 0.0, 0.0, 0.464942531101),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 39
    (2415, -90.465996, -0.000744, 1e-4, 0.0, 0.0, 3),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 40
    (2416, -62.533749, -0.000744, 1e-4, 0.0, 0.0, 2),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 41
    (2417, -131.188253, -0.000744, 1e-4, 0.0, 0.0, 4),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 42
    (2418, -80.128068, -0.000744, 1e-4, 0.0, 0.0, 3),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 43
    (2419, 42.824406, -0.000744, 1e-4, 0.0, 0.0, 40256829.9606),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 44
    (2420, -67.133334, -0.000744, 1e-4, 0.0, 0.0, 2),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger zone 45
    (2421, 115.712464, -0.000744, 1e-4, 0.0, 0.0, 64966193.2191),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 75E
    (2422, 70.512527, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 78E
    (2423, 73.512594, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 81E
    (2424, 76.512662, -0.000744, 1e-4, 0.0, 0.0, 0.000167353166099),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 84E
    (2425, 79.512732, -0.000744, 1e-4, 0.0, 0.0, 0.000167353166034),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 87E
    (2426, 82.512802, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 90E
    (2427, 85.512874, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 93E
    (2428, 88.512946, -0.000744, 1e-4, 0.0, 0.0, 0.000167358055499),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 96E
    (2429, 91.513018, -0.000744, 1e-4, 0.0, 0.0, 0.000167353864548),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 99E
    (2430, 94.513091, -0.000744, 1e-4, 0.0, 0.0, 0.000167353864526),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 102E
    (2431, 97.513164, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 105E
    (2432, 100.513237, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 108E
    (2433, 103.513310, -0.000744, 1e-4, 0.0, 0.0, 0.000167353166056),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 111E
    (2434, 106.513382, -0.000744, 1e-4, 0.0, 0.0, 0.000167356658515),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 114E
    (2435, 109.513453, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 117E
    (2436, 112.513524, -0.000744, 1e-4, 0.0, 0.0, 0.000167351769072),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 120E
    (2437, 115.513593, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 123E
    (2438, 118.513661, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 126E
    (2439, 121.513728, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 129E
    (2440, 124.513793, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 132E
    (2441, 127.513856, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / 3-degree Gauss-Kruger CM 135E
    (2442, 130.513917, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS I
    (2443, 129.500000, 33.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS II
    (2444, 131.0, 33.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS III
    (2445, 132.166667, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS IV
    (2446, 133.500000, 33.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS V
    (2447, 134.333333, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS VI
    (2448, 136.0, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS VII
    (2449, 137.166667, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS VIII
    (2450, 138.500000, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS IX
    (2451, 139.833333, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS X
    (2452, 140.833333, 40.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS XI
    (2453, 140.250000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS XII
    (2454, 142.250000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS XIII
    (2455, 144.250000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS XIV
    (2456, 142.0, 26.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS XV
    (2457, 127.500000, 26.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS XVI
    (2458, 124.0, 26.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS XVII
    (2459, 131.0, 26.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS XVIII
    (2460, 136.0, 20.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2000 - JGD2000 / Japan Plane Rectangular CS XIX
    (2461, 154.0, 26.0, 1e-4, 0.0, 0.0, 1e-07),
    # Albanian 1987 - Albanian 1987 / Gauss-Kruger zone 4
    (2462, -16.406844, 0.000567, 2e-3, 0.0, 0.0, 2788),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 21E
    (2463, 16.511970, -0.000738, 1e-4, 0.0, 0.0, 0.000171152612893),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 27E
    (2464, 22.511989, -0.000738, 1e-4, 0.0, 0.0, 0.000171152612893),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 33E
    (2465, 28.512022, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 39E
    (2466, 34.512066, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057636),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 45E
    (2467, 40.512123, -0.000738, 1e-4, 0.0, 0.0, 0.00017115479568),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 51E
    (2468, 46.512191, -0.000738, 1e-4, 0.0, 0.0, 0.00017115479568),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 57E
    (2469, 52.512270, -0.000738, 1e-4, 0.0, 0.0, 0.000171154795744),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 63E
    (2470, 58.512358, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866706),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 69E
    (2471, 64.512455, -0.000738, 1e-4, 0.0, 0.0, 0.000171151565176),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 75E
    (2472, 70.512560, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866706),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 81E
    (2473, 76.512672, -0.000738, 1e-4, 0.0, 0.0, 0.00017114877123),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 87E
    (2474, 82.512789, -0.000738, 1e-4, 0.0, 0.0, 0.000171150168171),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 93E
    (2475, 88.512910, -0.000738, 1e-4, 0.0, 0.0, 0.00017115226369),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 99E
    (2476, 94.513033, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660674),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 105E
    (2477, 100.513158, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 111E
    (2478, 106.513283, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 117E
    (2479, 112.513407, -0.000738, 1e-4, 0.0, 0.0, 0.000171148771209),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 123E
    (2480, 118.513528, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 129E
    (2481, 124.513645, -0.000738, 1e-4, 0.0, 0.0, 0.000171149469722),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 135E
    (2482, 130.513757, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 141E
    (2483, 136.513862, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660652),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 147E
    (2484, 142.513959, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866727),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 153E
    (2485, 148.514048, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057615),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 159E
    (2486, 154.514127, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660674),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 165E
    (2487, 160.514195, -0.000738, 1e-4, 0.0, 0.0, 0.000171152263647),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 171E
    (2488, 166.514252, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057636),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 177E
    (2489, 172.514297, -0.000738, 1e-4, 0.0, 0.0, 0.00017115226369),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 177W
    (2490, 178.514330, -0.000738, 1e-4, 0.0, 0.0, 0.000171145191416),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger CM 171W
    (2491, -175.485651, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 9E (deprecated)
    (2492, 4.512070, -0.000829, 1e-4, 0.0, 0.0, 0.000348863587362),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 15E (deprecated)
    (2493, 10.512065, -0.000828, 1e-4, 0.0, 0.0, 0.000352046177593),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 21E
    (2494, 16.512074, -0.000825, 1e-4, 0.0, 0.0, 0.0003547533341),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 27E
    (2495, 22.512097, -0.000822, 1e-4, 0.0, 0.0, 0.000357078236813),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 33E
    (2496, 28.512134, -0.000818, 1e-4, 0.0, 0.0, 0.000359142031643),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 39E
    (2497, 34.512184, -0.000812, 1e-4, 0.0, 0.0, 0.000361041860741),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 45E
    (2498, 40.512247, -0.000806, 1e-4, 0.0, 0.0, 0.000362892720337),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 51E
    (2499, 46.512322, -0.000799, 1e-4, 0.0, 0.0, 0.000364781442513),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 57E
    (2500, 52.512409, -0.000791, 1e-4, 0.0, 0.0, 0.000366786985697),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 63E
    (2501, 58.512505, -0.000783, 1e-4, 0.0, 0.0, 0.000368971420167),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 69E
    (2502, 64.512611, -0.000774, 1e-4, 0.0, 0.0, 0.000371366311652),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 75E
    (2503, 70.512726, -0.000764, 1e-4, 0.0, 0.0, 0.000373968467309),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 81E
    (2504, 76.512847, -0.000754, 1e-4, 0.0, 0.0, 0.000376742069684),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 87E
    (2505, 82.512973, -0.000744, 1e-4, 0.0, 0.0, 0.000379647888205),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 93E
    (2506, 88.513104, -0.000734, 1e-4, 0.0, 0.0, 0.000382578025962),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 99E
    (2507, 94.513237, -0.000724, 1e-4, 0.0, 0.0, 0.000385434473965),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 105E
    (2508, 100.513372, -0.000714, 1e-4, 0.0, 0.0, 0.000388083772623),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 111E
    (2509, 106.513506, -0.000704, 1e-4, 0.0, 0.0, 0.000390382135813),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 117E
    (2510, 112.513639, -0.000694, 1e-4, 0.0, 0.0, 0.000392181631197),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 123E
    (2511, 118.513768, -0.000685, 1e-4, 0.0, 0.0, 0.000393307900105),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 129E
    (2512, 124.513893, -0.000676, 1e-4, 0.0, 0.0, 0.000393654881024),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 135E
    (2513, 130.514013, -0.000668, 1e-4, 0.0, 0.0, 0.000393044411281),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 141E
    (2514, 136.514124, -0.000661, 1e-4, 0.0, 0.0, 0.000391412736055),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 147E
    (2515, 142.514228, -0.000654, 1e-4, 0.0, 0.0, 0.000388648486567),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 153E
    (2516, 148.514321, -0.000648, 1e-4, 0.0, 0.0, 0.000384712435489),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 159E
    (2517, 154.514405, -0.000643, 1e-4, 0.0, 0.0, 0.000379595055897),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 165E
    (2518, 160.514476, -0.000639, 1e-4, 0.0, 0.0, 0.000373326502891),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 171E
    (2519, 166.514535, -0.000636, 1e-4, 0.0, 0.0, 0.000365976858996),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 177E
    (2520, 172.514582, -0.000635, 1e-4, 0.0, 0.0, 0.000357641063587),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 177W
    (2521, 178.514615, -0.000634, 1e-4, 0.0, 0.0, 0.000348491218294),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger CM 171W
    (2522, -175.485366, -0.000634, 1e-4, 0.0, 0.0, 0.000348862190418),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 7
    (2523, -33.997554, -0.000813, 1e-4, 0.0, 0.0, 228895),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 8
    (2524, -34.542191, -0.000812, 1e-4, 0.0, 0.0, 675426),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 9
    (2525, -32.938056, -0.000814, 1e-4, 0.0, 0.0, 1736227),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 10
    (2526, -27.808170, -0.000818, 1e-4, 0.0, 0.0, 3900661),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 11
    (2527, -16.859601, -0.000825, 1e-4, 0.0, 0.0, 7620250),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 12
    (2528, 3.497852, -0.000829, 1e-4, 0.0, 0.0, 13003845.6945),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 13
    (2529, 38.618532, -0.000808, 1e-4, 0.0, 0.0, 20186451.1939),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 14
    (2530, 96.181346, -0.000721, 1e-4, 0.0, 0.0, 32540211.601),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 15
    (2531, -173.149726, -0.000634, 1e-4, 0.0, 0.0, 5),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 16
    (2532, -34.955817, -0.000812, 1e-4, 0.0, 0.0, 1897470),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 17
    (2533, 169.841810, -0.000635, 1e-4, 0.0, 0.0, 5),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 18
    (2534, 105.970632, -0.000705, 1e-4, 0.0, 0.0, 37933969.7859),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 19
    (2535, 164.985459, -0.000637, 1e-4, 0.0, 0.0, 5),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 20
    (2536, 26.537573, -0.000819, 1e-4, 0.0, 0.0, 24812506.3052),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 21
    (2537, 99.841466, -0.000715, 1e-4, 0.0, 0.0, 38878499.3144),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 22
    (2538, 85.252027, -0.000740, 1e-4, 0.0, 0.0, 37027455.3706),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 23
    (2539, 56.052195, -0.000786, 1e-4, 0.0, 0.0, 33069293.0705),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 24
    (2540, 100.390695, -0.000714, 1e-4, 0.0, 0.0, 41698979.5418),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 25
    (2541, -36.596393, -0.000810, 1e-4, 0.0, 0.0, 0.958242926769),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 26
    (2542, 129.545135, -0.000669, 1e-4, 0.0, 0.0, 49820287.877),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 27
    (2543, 25.087568, -0.000820, 1e-4, 0.0, 0.0, 29965077.926),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 28
    (2544, -179.129791, -0.000634, 1e-4, 0.0, 0.0, 5),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 29
    (2545, 75.210008, -0.000757, 1e-4, 0.0, 0.0, 42267195.8386),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 30
    (2546, -62.902318, -0.000776, 1e-4, 0.0, 0.0, 2),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 31
    (2547, 29.570137, -0.000817, 1e-4, 0.0, 0.0, 33569247.8907),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 32
    (2548, -66.638085, -0.000770, 1e-4, 0.0, 0.0, 2),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 33
    (2549, -9.221763, -0.000828, 1e-4, 0.0, 0.0, 0.241577319378),
    # Samboja - Samboja / UTM zone 50S (deprecated)
    (2550, 120.554785, -89.992870, 1e-4, 0.0, 0.0, 3),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 34
    (2551, -130.149049, -0.000669, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 35
    (2552, 8.723305, -0.000828, 1e-4, 0.0, 0.0, 0.228581435546),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 36
    (2553, -179.642284, -0.000634, 1e-4, 0.0, 0.0, 71962992.2843),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 37
    (2554, -142.538747, -0.000654, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 38
    (2555, 17.758450, -0.000825, 1e-4, 0.0, 0.0, 0.465118721502),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 39
    (2556, -90.465697, -0.000731, 1e-4, 0.0, 0.0, 3),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 40
    (2557, -62.533402, -0.000777, 1e-4, 0.0, 0.0, 2),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 41
    (2558, -131.188048, -0.000667, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 42
    (2559, -80.127748, -0.000748, 1e-4, 0.0, 0.0, 3),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 43
    (2560, 42.824670, -0.000803, 1e-4, 0.0, 0.0, 40256829.9609),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 44
    (2561, -67.132993, -0.000770, 1e-4, 0.0, 0.0, 2),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 45
    (2562, 115.712575, -0.000689, 1e-4, 0.0, 0.0, 64966193.2193),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 46
    (2563, 155.448671, -0.000643, 1e-4, 0.0, 0.0, 72709820.3412),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 47
    (2564, 102.747025, -0.000710, 1e-4, 0.0, 0.0, 64323917.5223),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 48
    (2565, 143.455886, -0.000653, 1e-4, 0.0, 0.0, 72658954.5001),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 49
    (2566, -112.448310, -0.000694, 1e-4, 0.0, 0.0, 3),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 50
    (2567, 178.499964, -0.000634, 1e-4, 0.0, 0.0, 80719510.8968),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 51
    (2568, -137.008104, -0.000660, 1e-4, 0.0, 0.0, 93438223.5725),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 52
    (2569, 121.097070, -0.000681, 1e-4, 0.0, 0.0, 72521127.2725),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 53
    (2570, 156.821284, -0.000642, 1e-4, 0.0, 0.0, 79885886.3932),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 54
    (2571, 89.078085, -0.000733, 1e-4, 0.0, 0.0, 64281091.2851),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 55
    (2572, -116.894217, -0.000687, 1e-4, 0.0, 0.0, 103283483.746),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 56
    (2573, 82.291607, -0.000745, 1e-4, 0.0, 0.0, 60081632.1062),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 57
    (2574, 20.471122, -0.000823, 1e-4, 0.0, 0.0, 0.536135233806),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 58
    (2575, -5.344947, -0.000829, 1e-4, 0.0, 0.0, 0.140082317944),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 59
    (2576, -161.772650, -0.000639, 1e-4, 0.0, 0.0, 92878899.0837),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 60 (deprecated)
    (2577, 116.417542, -0.000688, 1e-4, 0.0, 0.0, 76265290.6422),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 61
    (2578, -168.628879, -0.000636, 1e-4, 0.0, 0.0, 93652605.2786),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 62
    (2579, -25.072981, -0.000820, 1e-4, 0.0, 0.0, 0.656561170984),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 63
    (2580, 5.579077, -0.000829, 1e-4, 0.0, 0.0, 0.14626605012),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 64
    (2581, 173.165013, -0.000634, 1e-4, 0.0, 0.0, 93546001.9511),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 21E
    (2582, 16.512074, -0.000825, 1e-4, 0.0, 0.0, 0.0003547533341),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 24E
    (2583, 19.512084, -0.000824, 1e-4, 0.0, 0.0, 0.000355962271936),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 27E
    (2584, 22.512097, -0.000822, 1e-4, 0.0, 0.0, 0.000357078236813),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 30E
    (2585, 25.512114, -0.000820, 1e-4, 0.0, 0.0, 0.000358139792105),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 33E
    (2586, 28.512134, -0.000818, 1e-4, 0.0, 0.0, 0.000359142031643),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 36E
    (2587, 31.512157, -0.000815, 1e-4, 0.0, 0.0, 0.000360105928401),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 39E
    (2588, 34.512184, -0.000812, 1e-4, 0.0, 0.0, 0.000361041860741),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 42E
    (2589, 37.512214, -0.000809, 1e-4, 0.0, 0.0, 0.000361967882921),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 45E
    (2590, 40.512247, -0.000806, 1e-4, 0.0, 0.0, 0.000362892720337),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 48E
    (2591, 43.512283, -0.000803, 1e-4, 0.0, 0.0, 0.000363823561053),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 51E
    (2592, 46.512322, -0.000799, 1e-4, 0.0, 0.0, 0.000364781442513),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 54E
    (2593, 49.512364, -0.000795, 1e-4, 0.0, 0.0, 0.000365767131329),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 57E
    (2594, 52.512409, -0.000791, 1e-4, 0.0, 0.0, 0.000366786985697),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 60E
    (2595, 55.512456, -0.000787, 1e-4, 0.0, 0.0, 0.000367860219464),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 63E
    (2596, 58.512505, -0.000783, 1e-4, 0.0, 0.0, 0.000368971420167),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 66E
    (2597, 61.512557, -0.000778, 1e-4, 0.0, 0.0, 0.0003701439936),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 69E
    (2598, 64.512611, -0.000774, 1e-4, 0.0, 0.0, 0.000371366311652),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 72E
    (2599, 67.512668, -0.000769, 1e-4, 0.0, 0.0, 0.000372640095883),
    # LKS94 - Lietuvos Koordinoei Sistema 1994 (deprecated)
    (2600, 19.512152, 0.0, 1e-4, 0.0, 0.0, 0.000167567079188),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 75E
    (2601, 70.512726, -0.000764, 1e-4, 0.0, 0.0, 0.000373968467309),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 78E
    (2602, 73.512785, -0.000759, 1e-4, 0.0, 0.0, 0.00037533491463),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 81E
    (2603, 76.512847, -0.000754, 1e-4, 0.0, 0.0, 0.000376742069684),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 84E
    (2604, 79.512909, -0.000749, 1e-4, 0.0, 0.0, 0.000378181156501),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 87E
    (2605, 82.512973, -0.000744, 1e-4, 0.0, 0.0, 0.000379647888205),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 90E
    (2606, 85.513038, -0.000739, 1e-4, 0.0, 0.0, 0.000381109556026),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 93E
    (2607, 88.513104, -0.000734, 1e-4, 0.0, 0.0, 0.000382578025962),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 96E
    (2608, 91.513170, -0.000729, 1e-4, 0.0, 0.0, 0.000384025485311),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 99E
    (2609, 94.513237, -0.000724, 1e-4, 0.0, 0.0, 0.000385434473965),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 102E
    (2610, 97.513304, -0.000719, 1e-4, 0.0, 0.0, 0.00038679395513),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 105E
    (2611, 100.513372, -0.000714, 1e-4, 0.0, 0.0, 0.000388083772623),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 108E
    (2612, 103.513439, -0.000709, 1e-4, 0.0, 0.0, 0.000389289927657),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 111E
    (2613, 106.513506, -0.000704, 1e-4, 0.0, 0.0, 0.000390382135813),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 114E
    (2614, 109.513573, -0.000699, 1e-4, 0.0, 0.0, 0.000391355556792),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 117E
    (2615, 112.513639, -0.000694, 1e-4, 0.0, 0.0, 0.000392181631197),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 120E
    (2616, 115.513704, -0.000689, 1e-4, 0.0, 0.0, 0.000392841251179),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 123E
    (2617, 118.513768, -0.000685, 1e-4, 0.0, 0.0, 0.000393307900105),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 126E
    (2618, 121.513832, -0.000680, 1e-4, 0.0, 0.0, 0.000393587369135),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 129E
    (2619, 124.513893, -0.000676, 1e-4, 0.0, 0.0, 0.000393654881024),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 132E
    (2620, 127.513954, -0.000672, 1e-4, 0.0, 0.0, 0.000393471404899),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 135E
    (2621, 130.514013, -0.000668, 1e-4, 0.0, 0.0, 0.000393044411281),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 138E
    (2622, 133.514069, -0.000664, 1e-4, 0.0, 0.0, 0.000392366738613),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 141E
    (2623, 136.514124, -0.000661, 1e-4, 0.0, 0.0, 0.000391412736055),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 144E
    (2624, 139.514177, -0.000657, 1e-4, 0.0, 0.0, 0.000390171948292),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 147E
    (2625, 142.514228, -0.000654, 1e-4, 0.0, 0.0, 0.000388648486567),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 150E
    (2626, 145.514276, -0.000651, 1e-4, 0.0, 0.0, 0.000386829141045),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 153E
    (2627, 148.514321, -0.000648, 1e-4, 0.0, 0.0, 0.000384712435489),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 156E
    (2628, 151.514364, -0.000646, 1e-4, 0.0, 0.0, 0.000382302495336),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 159E
    (2629, 154.514405, -0.000643, 1e-4, 0.0, 0.0, 0.000379595055897),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 162E
    (2630, 157.514442, -0.000641, 1e-4, 0.0, 0.0, 0.000376603896322),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 165E
    (2631, 160.514476, -0.000639, 1e-4, 0.0, 0.0, 0.000373326502891),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 168E
    (2632, 163.514507, -0.000638, 1e-4, 0.0, 0.0, 0.000369771636628),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 171E
    (2633, 166.514535, -0.000636, 1e-4, 0.0, 0.0, 0.000365976858996),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 174E
    (2634, 169.514560, -0.000635, 1e-4, 0.0, 0.0, 0.00036192390267),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 177E
    (2635, 172.514582, -0.000635, 1e-4, 0.0, 0.0, 0.000357641063587),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 180E
    (2636, 175.514600, -0.000634, 1e-4, 0.0, 0.0, 0.000353154281429),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 177W
    (2637, 178.514615, -0.000634, 1e-4, 0.0, 0.0, 0.000348491218294),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 174W
    (2638, -178.485374, -0.000634, 1e-4, 0.0, 0.0, 0.00034706887525),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 171W
    (2639, -175.485366, -0.000634, 1e-4, 0.0, 0.0, 0.000348862190418),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 168W
    (2640, -172.485362, -0.000635, 1e-4, 0.0, 0.0, 0.000350520616384),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 7
    (2641, -33.997666, -0.000738, 1e-4, 0.0, 0.0, 228895),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 8
    (2642, -34.542303, -0.000738, 1e-4, 0.0, 0.0, 675426),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 9
    (2643, -32.938167, -0.000738, 1e-4, 0.0, 0.0, 1736227),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 10
    (2644, -27.808277, -0.000738, 1e-4, 0.0, 0.0, 3900661),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 11
    (2645, -16.859702, -0.000738, 1e-4, 0.0, 0.0, 7620250),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 12
    (2646, 3.497753, -0.000738, 1e-4, 0.0, 0.0, 13003845.6944),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 13
    (2647, 38.618410, -0.000738, 1e-4, 0.0, 0.0, 20186451.1937),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 14
    (2648, 96.181139, -0.000738, 1e-4, 0.0, 0.0, 32540211.6008),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 15
    (2649, -173.150009, -0.000738, 1e-4, 0.0, 0.0, 5),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 16
    (2650, -34.955929, -0.000738, 1e-4, 0.0, 0.0, 1897470),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 17
    (2651, 169.841526, -0.000738, 1e-4, 0.0, 0.0, 5),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 18
    (2652, 105.970410, -0.000738, 1e-4, 0.0, 0.0, 37933969.7856),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 19
    (2653, 164.985176, -0.000738, 1e-4, 0.0, 0.0, 5),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 20
    (2654, 26.537462, -0.000738, 1e-4, 0.0, 0.0, 24812506.305),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 21
    (2655, 99.841253, -0.000738, 1e-4, 0.0, 0.0, 38878499.3142),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 22
    (2656, 85.251838, -0.000738, 1e-4, 0.0, 0.0, 37027455.3704),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 23
    (2657, 56.052051, -0.000738, 1e-4, 0.0, 0.0, 33069293.0703),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 24
    (2658, 100.390481, -0.000738, 1e-4, 0.0, 0.0, 41698979.5416),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 25
    (2659, -36.596506, -0.000738, 1e-4, 0.0, 0.0, 0.958077758569),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 26
    (2660, 129.544880, -0.000738, 1e-4, 0.0, 0.0, 49820287.8768),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 27
    (2661, 25.087458, -0.000738, 1e-4, 0.0, 0.0, 29965077.9258),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 28
    (2662, -179.130075, -0.000738, 1e-4, 0.0, 0.0, 5),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 29
    (2663, 75.209836, -0.000738, 1e-4, 0.0, 0.0, 42267195.8384),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 30
    (2664, -62.902462, -0.000738, 1e-4, 0.0, 0.0, 2),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 31
    (2665, 29.570024, -0.000738, 1e-4, 0.0, 0.0, 33569247.8905),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 32
    (2666, -66.638235, -0.000738, 1e-4, 0.0, 0.0, 2),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 33
    (2667, -9.221862, -0.000738, 1e-4, 0.0, 0.0, 0.24140307358),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 34
    (2668, -130.149297, -0.000738, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 35
    (2669, 8.723205, -0.000738, 1e-4, 0.0, 0.0, 0.228407115708),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 36
    (2670, -179.642568, -0.000738, 1e-4, 0.0, 0.0, 71962992.2841),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 37
    (2671, -142.539010, -0.000738, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 38
    (2672, 17.758346, -0.000738, 1e-4, 0.0, 0.0, 0.4649463295),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 39
    (2673, -90.465884, -0.000738, 1e-4, 0.0, 0.0, 3),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 40
    (2674, -62.533547, -0.000738, 1e-4, 0.0, 0.0, 2),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 41
    (2675, -131.188298, -0.000738, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 42
    (2676, -80.127918, -0.000738, 1e-4, 0.0, 0.0, 3),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 43
    (2677, 42.824544, -0.000738, 1e-4, 0.0, 0.0, 40256829.9606),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 44
    (2678, -67.133144, -0.000738, 1e-4, 0.0, 0.0, 2),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 45
    (2679, 115.712339, -0.000738, 1e-4, 0.0, 0.0, 64966193.2191),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 46
    (2680, 155.448393, -0.000738, 1e-4, 0.0, 0.0, 72709820.341),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 47
    (2681, 102.746808, -0.000738, 1e-4, 0.0, 0.0, 64323917.5221),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 48
    (2682, 143.455617, -0.000738, 1e-4, 0.0, 0.0, 72658954.4998),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 49
    (2683, -112.448533, -0.000738, 1e-4, 0.0, 0.0, 3),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 50
    (2684, 178.499679, -0.000738, 1e-4, 0.0, 0.0, 80719510.8967),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 51
    (2685, -137.008360, -0.000738, 1e-4, 0.0, 0.0, 93438223.5723),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 52
    (2686, 121.096826, -0.000738, 1e-4, 0.0, 0.0, 72521127.2723),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 53
    (2687, 156.821005, -0.000738, 1e-4, 0.0, 0.0, 79885886.393),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 54
    (2688, 89.077890, -0.000738, 1e-4, 0.0, 0.0, 64281091.2848),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 55
    (2689, -116.894447, -0.000738, 1e-4, 0.0, 0.0, 103283483.746),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 56
    (2690, 82.291423, -0.000738, 1e-4, 0.0, 0.0, 60081632.1057),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 57
    (2691, 20.471016, -0.000738, 1e-4, 0.0, 0.0, 0.535963652055),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 58
    (2692, -5.345046, -0.000738, 1e-4, 0.0, 0.0, 0.139907604157),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 59
    (2693, -161.772928, -0.000738, 1e-4, 0.0, 0.0, 92878899.0835),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 60 (deprecated)
    (2694, 116.417305, -0.000738, 1e-4, 0.0, 0.0, 76265290.6419),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 61
    (2695, -168.629161, -0.000738, 1e-4, 0.0, 0.0, 93652605.2784),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 62
    (2696, -25.073086, -0.000738, 1e-4, 0.0, 0.0, 0.65639116503),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 63
    (2697, 5.578978, -0.000738, 1e-4, 0.0, 0.0, 0.146091358685),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 64
    (2698, 173.164728, -0.000738, 1e-4, 0.0, 0.0, 93546001.9509),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 21E
    (2699, 16.511970, -0.000738, 1e-4, 0.0, 0.0, 0.000171152612893),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 24E
    (2700, 19.511978, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057615),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 27E
    (2701, 22.511989, -0.000738, 1e-4, 0.0, 0.0, 0.000171152612893),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 30E
    (2702, 25.512004, -0.000738, 1e-4, 0.0, 0.0, 0.000171152612893),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 33E
    (2703, 28.512022, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 36E
    (2704, 31.512043, -0.000738, 1e-4, 0.0, 0.0, 0.000171152612893),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 39E
    (2705, 34.512066, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057636),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 42E
    (2706, 37.512093, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866663),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 45E
    (2707, 40.512123, -0.000738, 1e-4, 0.0, 0.0, 0.00017115479568),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 48E
    (2708, 43.512156, -0.000738, 1e-4, 0.0, 0.0, 0.000171156454599),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 51E
    (2709, 46.512191, -0.000738, 1e-4, 0.0, 0.0, 0.00017115479568),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 54E
    (2710, 49.512229, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057636),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 57E
    (2711, 52.512270, -0.000738, 1e-4, 0.0, 0.0, 0.000171154795744),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 60E
    (2712, 55.512313, -0.000738, 1e-4, 0.0, 0.0, 0.000171152612893),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 63E
    (2713, 58.512358, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866706),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 66E
    (2714, 61.512406, -0.000738, 1e-4, 0.0, 0.0, 0.000171149469722),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 69E
    (2715, 64.512455, -0.000738, 1e-4, 0.0, 0.0, 0.000171151565176),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 72E
    (2716, 67.512507, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057636),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 75E
    (2717, 70.512560, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866706),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 78E
    (2718, 73.512615, -0.000738, 1e-4, 0.0, 0.0, 0.000171150168193),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 81E
    (2719, 76.512672, -0.000738, 1e-4, 0.0, 0.0, 0.00017114877123),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 84E
    (2720, 79.512730, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660631),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 87E
    (2721, 82.512789, -0.000738, 1e-4, 0.0, 0.0, 0.000171150168171),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 90E
    (2722, 85.512849, -0.000738, 1e-4, 0.0, 0.0, 0.00017115226369),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 93E
    (2723, 88.512910, -0.000738, 1e-4, 0.0, 0.0, 0.00017115226369),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 96E
    (2724, 91.512971, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057636),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 99E
    (2725, 94.513033, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660674),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 102E
    (2726, 97.513096, -0.000738, 1e-4, 0.0, 0.0, 0.000171150168193),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 105E
    (2727, 100.513158, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 108E
    (2728, 103.513221, -0.000738, 1e-4, 0.0, 0.0, 0.000171154446477),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 111E
    (2729, 106.513283, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 114E
    (2730, 109.513345, -0.000738, 1e-4, 0.0, 0.0, 0.000171152263668),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 117E
    (2731, 112.513407, -0.000738, 1e-4, 0.0, 0.0, 0.000171148771209),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 120E
    (2732, 115.513468, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660652),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 123E
    (2733, 118.513528, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 126E
    (2734, 121.513587, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660631),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 129E
    (2735, 124.513645, -0.000738, 1e-4, 0.0, 0.0, 0.000171149469722),
    # Tete - Tete / UTM zone 36S
    # (2736, -128.659808, -89.998853, 1e-4, 0.0, 0.0, 753170),
    # Tete - Tete / UTM zone 37S
    # (2737, -128.659808, -89.998853, 1e-4, 0.0, 0.0, 753170),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 132E
    (2738, 127.513701, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660652),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 135E
    (2739, 130.513757, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 138E
    (2740, 133.513810, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660631),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 141E
    (2741, 136.513862, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660652),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 144E
    (2742, 139.513911, -0.000738, 1e-4, 0.0, 0.0, 0.000171149469722),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 147E
    (2743, 142.513959, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866727),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 150E
    (2744, 145.514005, -0.000738, 1e-4, 0.0, 0.0, 0.000171143794475),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 153E
    (2745, 148.514048, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057615),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 156E
    (2746, 151.514088, -0.000738, 1e-4, 0.0, 0.0, 0.00017115645462),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 159E
    (2747, 154.514127, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660674),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 162E
    (2748, 157.514162, -0.000738, 1e-4, 0.0, 0.0, 0.000171156454599),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 165E
    (2749, 160.514195, -0.000738, 1e-4, 0.0, 0.0, 0.000171152263647),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 168E
    (2750, 163.514225, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057615),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 171E
    (2751, 166.514252, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057636),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 174E
    (2752, 169.514276, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660652),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 177E
    (2753, 172.514297, -0.000738, 1e-4, 0.0, 0.0, 0.00017115226369),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 180E
    (2754, 175.514315, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660631),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 177W
    (2755, 178.514330, -0.000738, 1e-4, 0.0, 0.0, 0.000171145191416),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 174W
    (2756, -178.485659, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057615),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 171W
    (2757, -175.485651, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 168W
    (2758, -172.485646, -0.000738, 1e-4, 0.0, 0.0, 0.000171157851604),
    # NAD83(HARN) - NAD83(HARN) / Alabama East
    (2759, -87.916194, 30.483360, 1e-4, 0.0, 0.0, 1.33309797832e-06),
    # NAD83(HARN) - NAD83(HARN) / Alabama West
    (2760, -93.703674, 29.853763, 1e-4, 0.0, 0.0, 0.00067773398073),
    # NAD83(HARN) - NAD83(HARN) / Arizona East
    (2761, -112.400208, 30.980684, 1e-4, 0.0, 0.0, 2.03809133589e-06),
    # NAD83(HARN) - NAD83(HARN) / Arizona Central
    (2762, -114.150208, 30.980684, 1e-4, 0.0, 0.0, 2.03625043994e-06),
    # NAD83(HARN) - NAD83(HARN) / Arizona West
    (2763, -115.983467, 30.980685, 1e-4, 0.0, 0.0, 2.03845919723e-06),
    # NAD83(HARN) - NAD83(HARN) / Arkansas North
    (2764, -96.343203, 34.253806, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Arkansas South
    (2765, -96.090762, 28.992599, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 1
    (2766, -143.321156, 32.649532, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 2
    (2767, -142.954019, 31.095993, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 3
    (2768, -141.218751, 30.009900, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 4
    (2769, -139.486472, 28.915572, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 5
    (2770, -138.152186, 27.196010, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 6
    (2771, -136.178771, 25.945490, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Colorado North
    (2772, -115.653598, 36.118325, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Colorado Central
    (2773, -115.464063, 34.638233, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Colorado South
    (2774, -115.330280, 33.489028, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Connecticut
    (2775, -76.287472, 39.405061, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Delaware
    (2776, -77.692913, 37.977968, 1e-4, 0.0, 0.0, 2.82291068248e-06),
    # NAD83(HARN) - NAD83(HARN) / Florida East
    (2777, -82.970337, 24.320543, 1e-4, 0.0, 0.0, 4.93879810895e-07),
    # NAD83(HARN) - NAD83(HARN) / Florida West
    (2778, -83.970337, 24.320543, 1e-4, 0.0, 0.0, 4.93879810895e-07),
    # NAD83(HARN) - NAD83(HARN) / Florida North
    (2779, -90.650782, 28.853971, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Georgia East
    (2780, -84.239141, 29.983687, 1e-4, 0.0, 0.0, 1.25848615201e-06),
    # NAD83(HARN) - NAD83(HARN) / Georgia West
    (2781, -91.398144, 29.801257, 1e-4, 0.0, 0.0, 0.00142275930678),
    # NAD83(HARN) - NAD83(HARN) / Hawaii zone 1
    (2782, -160.238151, 18.773126, 1e-4, 0.0, 0.0, 2.68067027011e-05),
    # NAD83(HARN) - NAD83(HARN) / Hawaii zone 2
    (2783, -161.448688, 20.267936, 1e-4, 0.0, 0.0, 5.14780165235e-05),
    # NAD83(HARN) - NAD83(HARN) / Hawaii zone 3
    (2784, -162.808055, 21.098347, 1e-4, 0.0, 0.0, 6.66313252259e-05),
    # NAD83(HARN) - NAD83(HARN) / Hawaii zone 4
    (2785, -164.329911, 21.762651, 1e-4, 0.0, 0.0, 7.90273515641e-05),
    # NAD83(HARN) - NAD83(HARN) / Hawaii zone 5
    (2786, -164.990986, 21.596578, 1e-4, 0.0, 0.0, 7.59037093138e-05),
    # NAD83(HARN) - NAD83(HARN) / Idaho East
    (2787, -114.567265, 41.641591, 1e-4, 0.0, 0.0, 3.94830305492e-06),
    # NAD83(HARN) - NAD83(HARN) / Idaho Central
    (2788, -119.988244, 41.510438, 1e-4, 0.0, 0.0, 0.000560564359692),
    # NAD83(HARN) - NAD83(HARN) / Idaho West
    (2789, -125.292410, 41.269025, 1e-4, 0.0, 0.0, 0.0284014806996),
    # NAD83(HARN) - NAD83(HARN) / Illinois East
    (2790, -91.686566, 36.619446, 1e-4, 0.0, 0.0, 2.51901485408e-05),
    # NAD83(HARN) - NAD83(HARN) / Illinois West
    (2791, -97.964340, 36.410947, 1e-4, 0.0, 0.0, 0.00255660697676),
    # NAD83(HARN) - NAD83(HARN) / Indiana East
    (2792, -86.765357, 35.241995, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Indiana West
    (2793, -96.907562, 34.847248, 1e-4, 0.0, 0.0, 0.01940148008),
    # NAD83(HARN) - NAD83(HARN) / Iowa North
    (2794, -109.055583, 31.312141, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Iowa South
    (2795, -99.345436, 39.848662, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Kansas North
    (2796, -102.570641, 38.242381, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Kansas South
    (2797, -102.765768, 32.984182, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Kentucky North
    (2798, -89.896811, 37.361875, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Kentucky South
    (2799, -91.003100, 31.708902, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Louisiana North
    (2800, -102.882630, 30.067691, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Louisiana South
    (2801, -101.517796, 28.098894, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Maine East
    (2802, -72.216114, 43.606225, 1e-4, 0.0, 0.0, 4.76777649198e-05),
    # NAD83(HARN) - NAD83(HARN) / Maine West
    (2803, -81.077225, 42.310713, 1e-4, 0.0, 0.0, 0.102337038947),
    # NAD83(HARN) - NAD83(HARN) / Maryland
    (2804, -81.529189, 37.577262, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Massachusetts Mainland
    (2805, -73.651391, 34.244387, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Massachusetts Island
    (2806, -76.433406, 40.845825, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Michigan North
    (2807, -158.790109, 11.663980, 1e-4, 0.0, 0.0, 2.29428967657e-07),
    # NAD83(HARN) - NAD83(HARN) / Michigan Central
    (2808, -144.322231, 22.874000, 1e-4, 0.0, 0.0, 2.08924502698e-07),
    # NAD83(HARN) - NAD83(HARN) / Michigan South
    (2809, -127.914179, 32.026364, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Minnesota North
    (2810, -103.287887, 45.126033, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Minnesota Central
    (2811, -104.182085, 43.648811, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Minnesota South
    (2812, -103.618869, 41.676372, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Mississippi East
    (2813, -91.925475, 29.464053, 1e-4, 0.0, 0.0, 1.2232144699e-05),
    # NAD83(HARN) - NAD83(HARN) / Mississippi West
    (2814, -97.529032, 29.305235, 1e-4, 0.0, 0.0, 0.00138477058454),
    # NAD83(HARN) - NAD83(HARN) / Missouri East
    (2815, -93.265663, 35.801506, 1e-4, 0.0, 0.0, 8.21736460543e-06),
    # NAD83(HARN) - NAD83(HARN) / Missouri Central
    (2816, -98.022683, 35.706341, 1e-4, 0.0, 0.0, 0.000399243352018),
    # NAD83(HARN) - NAD83(HARN) / Missouri West
    (2817, -103.890580, 35.797519, 1e-4, 0.0, 0.0, 0.0140642820106),
    # NAD83(HARN) - NAD83(HARN) / Montana
    (2818, -116.985465, 43.991943, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Nebraska
    (2819, -105.831722, 39.681418, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Nevada East
    (2820, -117.845072, -37.495818, 1e-4, 0.0, 0.0, 2.70277087111e-06),
    # NAD83(HARN) - NAD83(HARN) / Nevada Central
    (2821, -121.423274, -19.408016, 1e-4, 0.0, 0.0, 3.61900019925e-05),
    # NAD83(HARN) - NAD83(HARN) / Nevada West
    (2822, -125.753742, -1.377653, 1e-4, 0.0, 0.0, 0.00310022570193),
    # NAD83(HARN) - NAD83(HARN) / New Hampshire
    (2823, -75.312874, 42.441965, 1e-4, 0.0, 0.0, 4.28260943867e-05),
    # NAD83(HARN) - NAD83(HARN) / New Jersey
    (2824, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 6.02422172344e-07),
    # NAD83(HARN) - NAD83(HARN) / New Mexico East
    (2825, -106.060830, 30.988445, 1e-4, 0.0, 0.0, 4.79974244135e-07),
    # NAD83(HARN) - NAD83(HARN) / New Mexico Central
    (2826, -111.476675, 30.894188, 1e-4, 0.0, 0.0, 0.000274516973348),
    # NAD83(HARN) - NAD83(HARN) / New Mexico West
    (2827, -116.482847, 30.710010, 1e-4, 0.0, 0.0, 0.00241149640912),
    # NAD83(HARN) - NAD83(HARN) / New York East
    (2828, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 6.02422172344e-07),
    # NAD83(HARN) - NAD83(HARN) / New York Central
    (2829, -79.509326, 39.963054, 1e-4, 0.0, 0.0, 1.20960787919e-05),
    # NAD83(HARN) - NAD83(HARN) / New York West
    (2830, -82.677307, 39.927648, 1e-4, 0.0, 0.0, 8.10902805652e-05),
    # NAD83(HARN) - NAD83(HARN) / New York Long Island
    (2831, -77.519584, 40.112385, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / North Dakota North
    (2832, -108.360607, 46.724303, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / North Dakota South
    (2833, -108.173906, 45.402819, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Ohio North
    (2834, -89.475829, 39.450489, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Ohio South
    (2835, -89.316674, 37.795918, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Oklahoma North
    (2836, -104.561592, 34.817203, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Oklahoma South
    (2837, -104.434828, 33.160876, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Oregon North
    (2838, -150.091081, 39.506127, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Oregon South
    (2839, -138.236892, 40.231913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Rhode Island
    (2840, -72.689948, 41.077189, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / South Dakota North
    (2841, -107.437658, 43.585145, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / South Dakota South
    (2842, -107.593704, 42.097136, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Tennessee
    (2843, -92.508665, 34.153466, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas North
    (2844, -103.450598, 25.015254, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas North Central
    (2845, -103.763988, 13.819848, 1e-4, 0.0, 0.0, 1.59605406225e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas Central
    (2846, -105.983204, 3.442337, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas South Central
    (2847, -103.518030, -6.145758, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas South
    (2848, -100.642123, -15.495006, 1e-4, 0.0, 0.0, 2.08325218409e-07),
    # NAD83(HARN) - NAD83(HARN) / Utah North
    (2849, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Utah Central
    (2850, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 1.61089701578e-07),
    # NAD83(HARN) - NAD83(HARN) / Utah South
    (2851, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 1.55065208673e-07),
    # NAD83(HARN) - NAD83(HARN) / Vermont
    (2852, -78.566435, 42.339184, 1e-4, 0.0, 0.0, 0.000677358422587),
    # NAD83(HARN) - NAD83(HARN) / Virginia North
    (2853, -109.123093, 14.944862, 1e-4, 0.0, 0.0, 2.08849087358e-07),
    # NAD83(HARN) - NAD83(HARN) / Virginia South
    (2854, -111.898764, 21.848820, 1e-4, 0.0, 0.0, 1.84925738722e-07),
    # NAD83(HARN) - NAD83(HARN) / Washington North
    (2855, -127.390662, 46.808297, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Washington South
    (2856, -126.863697, 45.151783, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / West Virginia North
    (2857, -86.363854, 38.293447, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / West Virginia South
    (2858, -87.727921, 36.803713, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Wisconsin North
    (2859, -97.607760, 44.907938, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Wisconsin Central
    (2860, -97.438214, 43.585869, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Wisconsin South
    (2861, -97.222171, 41.765988, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Wyoming East
    (2862, -107.525253, 40.475927, 1e-4, 0.0, 0.0, 3.54957583129e-06),
    # NAD83(HARN) - NAD83(HARN) / Wyoming East Central
    (2863, -111.983494, 39.506205, 1e-4, 0.0, 0.0, 0.000162927870406),
    # NAD83(HARN) - NAD83(HARN) / Wyoming West Central
    (2864, -115.803136, 40.284354, 1e-4, 0.0, 0.0, 0.00182561155378),
    # NAD83(HARN) - NAD83(HARN) / Wyoming West
    (2865, -119.340930, 39.229368, 1e-4, 0.0, 0.0, 0.0180613152843),
    # NAD83(HARN) - NAD83(HARN) / Puerto Rico and Virgin Is.
    (2866, -68.300715, 16.017453, 1e-4, 0.0, 0.0, 1.52795109898e-07),
    # NAD83(HARN) - NAD83(HARN) / Arizona East (ft)
    (2867, -112.400208, 30.980684, 1e-4, 0.0, 0.0, 6.68665136447e-06),
    # NAD83(HARN) - NAD83(HARN) / Arizona Central (ft)
    (2868, -114.150208, 30.980684, 1e-4, 0.0, 0.0, 6.68061167961e-06),
    # NAD83(HARN) - NAD83(HARN) / Arizona West (ft)
    (2869, -115.983467, 30.980685, 1e-4, 0.0, 0.0, 6.68785825863e-06),
    # NAD83(HARN) - NAD83(HARN) / California zone 1 (ftUS)
    (2870, -143.321156, 32.649532, 1e-4, 0.0, 0.0, 2.79865998891e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 2 (ftUS)
    (2871, -142.954019, 31.095993, 1e-4, 0.0, 0.0, 3.30854891217e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 3 (ftUS)
    (2872, -141.218751, 30.009900, 1e-4, 0.0, 0.0, 3.35438162438e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 4 (ftUS)
    (2873, -139.486472, 28.915572, 1e-4, 0.0, 0.0, 3.79552147933e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 5 (ftUS)
    (2874, -138.152186, 27.196010, 1e-4, 0.0, 0.0, 4.19082862209e-07),
    # NAD83(HARN) - NAD83(HARN) / California zone 6 (ftUS)
    (2875, -136.178771, 25.945490, 1e-4, 0.0, 0.0, 4.50306397397e-07),
    # NAD83(HARN) - NAD83(HARN) / Colorado North (ftUS)
    (2876, -115.653598, 36.118325, 1e-4, 0.0, 0.0, 1.83330848813e-07),
    # NAD83(HARN) - NAD83(HARN) / Colorado Central (ftUS)
    (2877, -115.464063, 34.638233, 1e-4, 0.0, 0.0, 1.92210936802e-07),
    # NAD83(HARN) - NAD83(HARN) / Colorado South (ftUS)
    (2878, -115.330280, 33.489028, 1e-4, 0.0, 0.0, 2.39189466811e-07),
    # NAD83(HARN) - NAD83(HARN) / Connecticut (ftUS)
    (2879, -76.287472, 39.405061, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Delaware (ftUS)
    (2880, -77.692913, 37.977968, 1e-4, 0.0, 0.0, 9.25010817428e-06),
    # NAD83(HARN) - NAD83(HARN) / Florida East (ftUS)
    (2881, -82.970337, 24.320543, 1e-4, 0.0, 0.0, 1.6175350876e-06),
    # NAD83(HARN) - NAD83(HARN) / Florida West (ftUS)
    (2882, -83.970337, 24.320543, 1e-4, 0.0, 0.0, 1.61132241679e-06),
    # NAD83(HARN) - NAD83(HARN) / Florida North (ftUS)
    (2883, -90.650782, 28.853971, 1e-4, 0.0, 0.0, 3.25091364585e-07),
    # NAD83(HARN) - NAD83(HARN) / Georgia East (ftUS)
    (2884, -84.239141, 29.983687, 1e-4, 0.0, 0.0, 4.12373395515e-06),
    # NAD83(HARN) - NAD83(HARN) / Georgia West (ftUS)
    (2885, -91.398144, 29.801257, 1e-4, 0.0, 0.0, 0.00466783311378),
    # NAD83(HARN) - NAD83(HARN) / Idaho East (ftUS)
    (2886, -114.567265, 41.641591, 1e-4, 0.0, 0.0, 1.2961154679e-05),
    # NAD83(HARN) - NAD83(HARN) / Idaho Central (ftUS)
    (2887, -119.988244, 41.510438, 1e-4, 0.0, 0.0, 0.00183911055059),
    # NAD83(HARN) - NAD83(HARN) / Idaho West (ftUS)
    (2888, -125.292410, 41.269025, 1e-4, 0.0, 0.0, 0.0931805209225),
    # NAD83(HARN) - NAD83(HARN) / Indiana East (ftUS) (deprecated)
    (2889, -86.765434, 35.247718, 1e-4, 0.0, 0.0, 1.52608608914e-07),
    # NAD83(HARN) - NAD83(HARN) / Indiana West (ftUS) (deprecated)
    (2890, -96.908239, 34.852888, 1e-4, 0.0, 0.0, 0.0637416545774),
    # NAD83(HARN) - NAD83(HARN) / Kentucky North (ftUS)
    (2891, -89.896811, 37.361875, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Kentucky South (ftUS)
    (2892, -91.003100, 31.708902, 1e-4, 0.0, 0.0, 2.81011816696e-07),
    # NAD83(HARN) - NAD83(HARN) / Maryland (ftUS)
    (2893, -81.529189, 37.577262, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Massachusetts Mainland (ftUS)
    (2894, -73.651391, 34.244387, 1e-4, 0.0, 0.0, 2.03525887628e-07),
    # NAD83(HARN) - NAD83(HARN) / Massachusetts Island (ftUS)
    (2895, -76.433406, 40.845825, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Michigan North (ft)
    (2896, -158.790109, 11.663980, 1e-4, 0.0, 0.0, 7.44651609622e-07),
    # NAD83(HARN) - NAD83(HARN) / Michigan Central (ft)
    (2897, -144.322231, 22.874000, 1e-4, 0.0, 0.0, 6.80864563111e-07),
    # NAD83(HARN) - NAD83(HARN) / Michigan South (ft)
    (2898, -127.914179, 32.026364, 1e-4, 0.0, 0.0, 3.67591261454e-07),
    # NAD83(HARN) - NAD83(HARN) / Mississippi East (ftUS)
    (2899, -91.925475, 29.464053, 1e-4, 0.0, 0.0, 4.01310551576e-05),
    # NAD83(HARN) - NAD83(HARN) / Mississippi West (ftUS)
    (2900, -97.529032, 29.305235, 1e-4, 0.0, 0.0, 0.00454319441734),
    # NAD83(HARN) - NAD83(HARN) / Montana (ft)
    (2901, -116.985465, 43.991943, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / New Mexico East (ftUS)
    (2902, -106.060830, 30.988445, 1e-4, 0.0, 0.0, 1.5747154993e-06),
    # NAD83(HARN) - NAD83(HARN) / New Mexico Central (ftUS)
    (2903, -111.476675, 30.894188, 1e-4, 0.0, 0.0, 0.000900648487301),
    # NAD83(HARN) - NAD83(HARN) / New Mexico West (ftUS)
    (2904, -116.482847, 30.710010, 1e-4, 0.0, 0.0, 0.00791171227234),
    # NAD83(HARN) - NAD83(HARN) / New York East (ftUS)
    (2905, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 1.97644674377e-06),
    # NAD83(HARN) - NAD83(HARN) / New York Central (ftUS)
    (2906, -79.509326, 39.963054, 1e-4, 0.0, 0.0, 3.96810172177e-05),
    # NAD83(HARN) - NAD83(HARN) / New York West (ftUS)
    (2907, -82.677307, 39.927648, 1e-4, 0.0, 0.0, 0.000266040889871),
    # NAD83(HARN) - NAD83(HARN) / New York Long Island (ftUS)
    (2908, -77.519584, 40.112385, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / North Dakota North (ft)
    (2909, -108.360607, 46.724303, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / North Dakota South (ft)
    (2910, -108.173906, 45.402819, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Oklahoma North (ftUS)
    (2911, -104.561592, 34.817203, 1e-4, 0.0, 0.0, 1.91522645799e-07),
    # NAD83(HARN) - NAD83(HARN) / Oklahoma South (ftUS)
    (2912, -104.434828, 33.160876, 1e-4, 0.0, 0.0, 2.41455848176e-07),
    # NAD83(HARN) - NAD83(HARN) / Oregon North (ft)
    (2913, -150.091081, 39.506127, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Oregon South (ft)
    (2914, -138.236892, 40.231913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Tennessee (ftUS)
    (2915, -92.508665, 34.153466, 1e-4, 0.0, 0.0, 1.98492272166e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas North (ftUS)
    (2916, -103.450598, 25.015254, 1e-4, 0.0, 0.0, 4.1378345486e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas North Central (ftUS)
    (2917, -103.763988, 13.819848, 1e-4, 0.0, 0.0, 5.39107277291e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas Central (ftUS)
    (2918, -105.983204, 3.442337, 1e-4, 0.0, 0.0, 1.89059937838e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas South Central (ftUS)
    (2919, -103.518030, -6.145758, 1e-4, 0.0, 0.0, 3.59786790796e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas South (ftUS)
    (2920, -100.642123, -15.495006, 1e-4, 0.0, 0.0, 6.52829694445e-07),
    # NAD83(HARN) - NAD83(HARN) / Utah North (ft)
    (2921, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 2.76429098334e-07),
    # NAD83(HARN) - NAD83(HARN) / Utah Central (ft)
    (2922, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 5.19629413863e-07),
    # NAD83(HARN) - NAD83(HARN) / Utah South (ft)
    (2923, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 4.84108990865e-07),
    # NAD83(HARN) - NAD83(HARN) / Virginia North (ftUS)
    (2924, -109.123093, 14.944862, 1e-4, 0.0, 0.0, 6.84053229634e-07),
    # NAD83(HARN) - NAD83(HARN) / Virginia South (ftUS)
    (2925, -111.898764, 21.848820, 1e-4, 0.0, 0.0, 6.19314523647e-07),
    # NAD83(HARN) - NAD83(HARN) / Washington North (ftUS)
    (2926, -127.390662, 46.808297, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Washington South (ftUS)
    (2927, -126.863697, 45.151783, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Wisconsin North (ftUS)
    (2928, -97.607760, 44.907938, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Wisconsin Central (ftUS)
    (2929, -97.438214, 43.585869, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Wisconsin South (ftUS)
    (2930, -97.222171, 41.765988, 1e-4, 0.0, 0.0, 1e-07),
    # Beduaram - Beduaram / TM 13 NE
    (2931, 8.510703, 0.001700, 1e-4, 0.0, 0.0, 0.000172128493432),
    # QND95 - QND95 / Qatar National Grid
    (2932, 49.282984, 21.730409, 1e-4, 0.0, 0.0, 0.000645209394861),
    # Segara - Segara / UTM zone 50S
    (2933, 120.505796, -89.992891, 1e-4, 0.0, 0.0, 3),
    # Segara (Jakarta) - Segara (Jakarta) / NEIEZ (deprecated)
    (2934, -178.342383, -8.136056, 1e-4, 0.0, 0.0, 1e-07),
    # Pulkovo 1942 - Pulkovo 1942 / CS63 zone A1
    (2935, 29.934992, 0.113453, 1e-4, 0.0, 0.0, 0.0968360703003),
    # Pulkovo 1942 - Pulkovo 1942 / CS63 zone A2
    (2936, 24.308036, 0.108604, 1e-4, 0.0, 0.0, 10),
    # Pulkovo 1942 - Pulkovo 1942 / CS63 zone A3
    (2937, 19.137454, 0.101707, 1e-4, 0.0, 0.0, 197),
    # Pulkovo 1942 - Pulkovo 1942 / CS63 zone A4
    (2938, 14.559767, 0.093444, 1e-4, 0.0, 0.0, 1888),
    # Pulkovo 1942 - Pulkovo 1942 / CS63 zone K2
    (2939, 30.541400, 0.124241, 1e-4, 0.0, 0.0, 10),
    # Pulkovo 1942 - Pulkovo 1942 / CS63 zone K3
    (2940, 25.370803, 0.116359, 1e-4, 0.0, 0.0, 197),
    # Pulkovo 1942 - Pulkovo 1942 / CS63 zone K4
    (2941, 20.793103, 0.106914, 1e-4, 0.0, 0.0, 1888),
    # Porto Santo - Porto Santo / UTM zone 28N
    (2942, -19.492172, 0.002840, 1e-4, 0.0, 0.0, 0.000168836151801),
    # Selvagem Grande - Selvagem Grande / UTM zone 28N
    (2943, -19.490484, 0.000543, 1e-4, 0.0, 0.0, 0.000168836675584),
    # NAD83(CSRS) - NAD83(CSRS) / SCoPQ zone 2 (deprecated)
    (2944, -58.237290, 0.0, 1e-4, 0.0, 0.0, 1.07595697045e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 3
    (2945, -61.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 4
    (2946, -64.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 5
    (2947, -67.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 6
    (2948, -70.237290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 7
    (2949, -73.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 8
    (2950, -76.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 9
    (2951, -79.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 10
    (2952, -82.237290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83(CSRS) - NAD83(CSRS) / New Brunswick Stereographic
    (2953, -83.554088, -15.483645, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Prince Edward Isl. Stereographic (NAD83)
    (2954, -67.663924, 39.957439, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 11N
    (2955, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 12N
    (2956, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 13N
    (2957, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 17N
    (2958, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 18N
    (2959, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 19N
    (2960, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 20N
    (2961, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 21N
    (2962, -61.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD27 - NAD27 / Alaska Albers
    (2964, -154.0, 50.0, 2e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Indiana East (ftUS)
    (2965, -86.765357, 35.241995, 1e-4, 0.0, 0.0, 1.5225054085e-07),
    # NAD83 - NAD83 / Indiana West (ftUS)
    (2966, -96.907562, 34.847248, 1e-4, 0.0, 0.0, 0.0636530351664),
    # NAD83(HARN) - NAD83(HARN) / Indiana East (ftUS)
    (2967, -86.765357, 35.241995, 1e-4, 0.0, 0.0, 1.5225054085e-07),
    # NAD83(HARN) - NAD83(HARN) / Indiana West (ftUS)
    (2968, -96.907562, 34.847248, 1e-4, 0.0, 0.0, 0.0636530351664),
    # Fort Marigot - Fort Marigot / UTM zone 20N
    (2969, -67.486578, -0.003889, 1e-4, 0.0, 0.0, 0.000168836675584),
    # Guadeloupe 1948 - Guadeloupe 1948 / UTM zone 20N
    (2970, -67.492498, -0.002713, 1e-4, 0.0, 0.0, 0.000168838072654),
    # CSG67 - CSG67 / UTM zone 22N
    (2971, -55.488774, 0.000995, 1e-4, 0.0, 0.0, 0.000168834143572),
    # RGFG95 - RGFG95 / UTM zone 22N
    (2972, -55.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # Martinique 1938 - Martinique 1938 / UTM zone 20N
    (2973, -67.485366, 0.001366, 1e-4, 0.0, 0.0, 0.000168838072696),
    # RGR92 - RGR92 / UTM zone 40S
    (2975, 57.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Tahiti 52 - Tahiti 52 / UTM zone 6S
    (2976, 35.837653, -89.998211, 1e-4, 0.0, 0.0, 3),
    # Tahaa 54 - Tahaa 54 / UTM zone 5S
    (2977, 83.471422, -89.996437, 1e-4, 0.0, 0.0, 4),
    # IGN72 Nuku Hiva - IGN72 Nuku Hiva / UTM zone 7S
    (2978, 72.956066, -89.997434, 1e-4, 0.0, 0.0, 3),
    # K0 1949 - K0 1949 / UTM zone 42S (deprecated)
    (2979, -52.209962, -89.997881, 1e-4, 0.0, 0.0, 752569),
    # Combani 1950 - Combani 1950 / UTM zone 38S
    (2980, -171.220028, -89.996540, 1e-4, 0.0, 0.0, 752569),
    # IGN56 Lifou - IGN56 Lifou / UTM zone 58S
    (2981, 33.563759, -89.996396, 1e-4, 0.0, 0.0, 3),
    # IGN72 Grand Terre - IGN72 Grand Terre / UTM zone 58S (deprecated)
    (2982, -92.139365, -89.996882, 1e-4, 0.0, 0.0, 3),
    # ST87 Ouvea - ST87 Ouvea / UTM zone 58S (deprecated)
    (2983, -69.130477, -89.999229, 1e-4, 0.0, 0.0, 752569),
    # RGNC 1991 - RGNC 1991 / Lambert New Caledonia (deprecated)
    (2984, 162.067481, -24.162232, 1e-4, 0.0, 0.0, 1e-07),
    # Saint Pierre et Miquelon 1950 / UTM zone 21N
    (2987, -61.486614, 0.003328, 1e-4, 0.0, 0.0, 0.000170723476998),
    # MOP78 - MOP78 / UTM zone 1S
    (2988, -27.552812, -89.997445, 1e-4, 0.0, 0.0, 3),
    # RRAF 1991 - RRAF 1991 / UTM zone 20N (deprecated)
    (2989, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # Reunion 1947 - Reunion 1947 / TM Reunion (deprecated)
    (2990, 55.041188, -22.573334, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Oregon LCC (m)
    (2991, -125.300329, 41.644767, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Oregon GIC Lambert (ft)
    (2992, -125.300329, 41.644767, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Oregon LCC (m)
    (2993, -125.300329, 41.644767, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Oregon GIC Lambert (ft)
    (2994, -125.300329, 41.644767, 1e-4, 0.0, 0.0, 1e-07),
    # IGN53 Mare - IGN53 Mare / UTM zone 58S
    (2995, 31.724075, -89.996973, 1e-4, 0.0, 0.0, 3),
    # ST84 Ile des Pins - ST84 Ile des Pins / UTM zone 58S
    (2996, -92.139365, -89.996882, 1e-4, 0.0, 0.0, 3),
    # ST71 Belep - ST71 Belep / UTM zone 58S
    (2997, 176.658058, -89.990118, 1e-4, 0.0, 0.0, 752570),
    # NEA74 Noumea - NEA74 Noumea / UTM zone 58S
    (2998, -91.663976, -89.996861, 1e-4, 0.0, 0.0, 3),
    # Grand Comoros - Grand Comoros / UTM zone 38S
    (2999, 152.094529, -89.990245, 1e-4, 0.0, 0.0, 752569),
    # Segara - Segara / NEIEZ
    (3000, 74.861360, -8.135836, 1e-4, 0.0, 0.0, 1e-07),
    # Batavia - Batavia / NEIEZ
    (3001, 74.861125, -8.136645, 1e-4, 0.0, 0.0, 1e-07),
    # Makassar - Makassar / NEIEZ
    (3002, 74.862589, -8.135162, 1e-4, 0.0, 0.0, 1e-07),
    # Monte Mario - Monte Mario / Italy zone 1
    (3003, -4.356422, 0.000703, 1e-4, 0.0, 0.0, 0.295115295555),
    # Monte Mario - Monte Mario / Italy zone 2
    (3004, -7.074640, 0.000687, 1e-4, 0.0, 0.0, 21),
    # NAD83 - NAD83 / BC Albers
    (3005, -138.445861, 44.199437, 1e-4, 0.0, 0.0, 1e-07),
    # SWEREF99 - SWEREF99 TM
    (3006, 10.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # SWEREF99 - SWEREF99 12 00
    (3007, 10.652652, 0.0, 1e-4, 0.0, 0.0, 2.71800672635e-07),
    # SWEREF99 - SWEREF99 13 30
    (3008, 12.152652, 0.0, 1e-4, 0.0, 0.0, 2.71800672635e-07),
    # SWEREF99 - SWEREF99 15 00
    (3009, 13.652652, 0.0, 1e-4, 0.0, 0.0, 2.72106262855e-07),
    # SWEREF99 - SWEREF99 16 30
    (3010, 15.152652, 0.0, 1e-4, 0.0, 0.0, 2.72106262855e-07),
    # SWEREF99 - SWEREF99 18 00
    (3011, 16.652652, 0.0, 1e-4, 0.0, 0.0, 2.71538738161e-07),
    # SWEREF99 - SWEREF99 14 15
    (3012, 12.902652, 0.0, 1e-4, 0.0, 0.0, 2.72106262855e-07),
    # SWEREF99 - SWEREF99 15 45
    (3013, 14.402652, 0.0, 1e-4, 0.0, 0.0, 2.72106262855e-07),
    # SWEREF99 - SWEREF99 17 15
    (3014, 15.902652, 0.0, 1e-4, 0.0, 0.0, 2.72106262855e-07),
    # SWEREF99 - SWEREF99 18 45
    (3015, 17.402652, 0.0, 1e-4, 0.0, 0.0, 2.72106262855e-07),
    # SWEREF99 - SWEREF99 20 15
    (3016, 18.902652, 0.0, 1e-4, 0.0, 0.0, 2.72106262855e-07),
    # SWEREF99 - SWEREF99 21 45
    (3017, 20.402652, 0.0, 1e-4, 0.0, 0.0, 2.72106262855e-07),
    # SWEREF99 - SWEREF99 23 15
    (3018, 21.902652, 0.0, 1e-4, 0.0, 0.0, 2.71538738161e-07),
    # RT90 - RT90 7.5 gon V
    (3019, -2.046076, 0.004865, 1e-4, 0.0, 0.0, 0.30461599675),
    # RT90 - RT90 5 gon V
    (3020, 0.203778, 0.004855, 1e-4, 0.0, 0.0, 0.304479933297),
    # RT90 - RT90 2.5 gon V
    (3021, 2.453631, 0.004846, 1e-4, 0.0, 0.0, 0.304336469826),
    # RT90 - RT90 0 gon
    (3022, 4.703485, 0.004838, 1e-4, 0.0, 0.0, 0.304185857774),
    # RT90 - RT90 2.5 gon O
    (3023, 6.953338, 0.004831, 1e-4, 0.0, 0.0, 0.304028380858),
    # RT90 - RT90 5 gon O
    (3024, 9.203191, 0.004825, 1e-4, 0.0, 0.0, 0.30386434741),
    # RT38 - RT38 7.5 gon V
    (3025, -2.044629, 0.0, 1e-4, 0.0, 0.0, 0.290561034693),
    # RT38 - RT38 5 gon V
    (3026, 0.205371, 0.0, 1e-4, 0.0, 0.0, 0.290561034693),
    # RT38 - RT38 2.5 gon V
    (3027, 2.455371, 0.0, 1e-4, 0.0, 0.0, 0.290561034693),
    # RT38 - RT38 0 gon
    (3028, 4.705371, 0.0, 1e-4, 0.0, 0.0, 0.290561034693),
    # RT38 - RT38 2.5 gon O
    (3029, 6.955371, 0.0, 1e-4, 0.0, 0.0, 0.290561034693),
    # RT38 - RT38 5 gon O
    (3030, 9.205371, 0.0, 1e-4, 0.0, 0.0, 0.290561034693),
    # WGS 84 - WGS 84 / Antarctic Polar Stereographic
    (3031, 0.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / Australian Antarctic Polar Stereographic
    (3032, -65.0, -21.221413, 1e-4, 0.0, 0.0, 2.89175659418e-07),
    # WGS 84 - WGS 84 / Australian Antarctic Lambert
    (3033, -38.923471, -36.972866, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / LCC Europe
    (3034, -25.397641, 18.366885, 1e-4, 0.0, 0.0, 2.28406861424e-07),
    # ETRS89 - ETRS89 / LAEA Europe
    (3035, -29.086835, 12.993574, 1e-4, 0.0, 0.0, 0.00240807747468),
    # Moznet - Moznet / UTM zone 36S
    (3036, 33.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Moznet - Moznet / UTM zone 37S
    (3037, 39.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # ETRS89 - ETRS89 / TM26 (deprecated)
    (3038, -31.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / TM27 (deprecated)
    (3039, -25.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 28N (N-E)
    (3040, -19.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 29N (N-E)
    (3041, -13.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 30N (N-E)
    (3042, -7.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # ETRS89 - ETRS89 / UTM zone 31N (N-E)
    (3043, -1.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # ETRS89 - ETRS89 / UTM zone 32N (N-E)
    (3044, 4.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # ETRS89 - ETRS89 / UTM zone 33N (N-E)
    (3045, 10.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # ETRS89 - ETRS89 / UTM zone 34N (N-E)
    (3046, 16.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 35N (N-E)
    (3047, 22.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 36N (N-E)
    (3048, 28.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 37N (N-E)
    (3049, 34.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / TM38 (deprecated)
    (3050, 40.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / TM39 (deprecated)
    (3051, 46.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # Hjorsey 1955 - Hjorsey 1955 / UTM zone 26N
    (3054, -31.488550, -0.000751, 1e-4, 0.0, 0.0, 0.000168835540556),
    # Hjorsey 1955 - Hjorsey 1955 / UTM zone 27N
    (3055, -25.488469, -0.000751, 1e-4, 0.0, 0.0, 0.000168834754732),
    # Hjorsey 1955 - Hjorsey 1955 / UTM zone 28N
    (3056, -19.488388, -0.000751, 1e-4, 0.0, 0.0, 0.000168838945683),
    # ISN93 - ISN93 / Lambert 1993
    (3057, -28.015952, 60.198426, 1e-4, 0.0, 0.0, 2.39931978285e-07),
    # Helle 1954 - Helle 1954 / Jan Mayen Grid
    (3058, -9.822475, 70.270050, 1e-4, 0.0, 0.0, 0.431358823851),
    # LKS92 - LKS92 / Latvia TM
    (3059, 16.382391, 53.906101, 1e-4, 0.0, 0.0, 0.00750318102655),
    # IGN72 Grande Terre - IGN72 Grande Terre / UTM zone 58S
    (3060, -91.912436, -89.996877, 1e-4, 0.0, 0.0, 3),
    # Porto Santo 1995 - Porto Santo 1995 / UTM zone 28N
    (3061, -19.492170, 0.002828, 1e-4, 0.0, 0.0, 0.000168836675584),
    # Azores Oriental 1995 - Azores Oriental 1995 / UTM zone 26N
    (3062, -31.488454, 0.000499, 1e-4, 0.0, 0.0, 0.000168835540567),
    # Azores Central 1995 - Azores Central 1995 / UTM zone 26N
    (3063, -31.487791, -0.000343, 1e-4, 0.0, 0.0, 0.000168836675584),
    # IGM95 - IGM95 / UTM zone 32N
    (3064, 4.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728518136),
    # IGM95 - IGM95 / UTM zone 33N
    (3065, 10.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728518136),
    # ED50 - ED50 / Jordan TM
    (3066, 31.963953, 27.025276, 1e-4, 0.0, 0.0, 0.000186962104635),
    # ETRS89 - ETRS89 / TM35FIN(E,N)
    (3067, 22.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # DHDN - DHDN / Soldner Berlin
    (3068, 13.038727, 52.325925, 1e-4, 0.0, 0.0, 0.00129634073801),
    # NAD27 - NAD27 / Wisconsin Transverse Mercator
    (3069, -95.899196, 40.501862, 3e-4, 0.0, 0.0, 0.000545366870938),
    # NAD83 - NAD83 / Wisconsin Transverse Mercator
    (3070, -96.117596, 40.308550, 1e-4, 0.0, 0.0, 0.000626261142315),
    # NAD83(HARN) - NAD83(HARN) / Wisconsin Transverse Mercator
    (3071, -96.117596, 40.308550, 1e-4, 0.0, 0.0, 0.000626261142315),
    # NAD83 - NAD83 / Maine CS2000 East
    (3072, -76.529297, 43.504477, 1e-4, 0.0, 0.0, 0.0142061173473),
    # NAD83 - NAD83 / Maine CS2000 Central (deprecated)
    (3073, -75.239927, 42.836375, 1e-4, 0.0, 0.0, 0.00076136191455),
    # NAD83 - NAD83 / Maine CS2000 West
    (3074, -74.040638, 42.774624, 1e-4, 0.0, 0.0, 4.41511791914e-05),
    # NAD83(HARN) - NAD83(HARN) / Maine CS2000 East
    (3075, -76.529297, 43.504477, 1e-4, 0.0, 0.0, 0.0142061173473),
    # NAD83(HARN) - NAD83(HARN) / Maine CS2000 Central (deprecated)
    (3076, -75.239927, 42.836375, 1e-4, 0.0, 0.0, 0.00076136191455),
    # NAD83(HARN) - NAD83(HARN) / Maine CS2000 West
    (3077, -74.040638, 42.774624, 1e-4, 0.0, 0.0, 4.41511791914e-05),
    # NAD83 - NAD83 / Michigan Oblique Mercator
    (3078, -91.882286, 40.401569, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Michigan Oblique Mercator
    (3079, -91.882286, 40.401569, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Shackleford
    (3080, -108.824391, 22.597716, 4e-4, 0.0, 0.0, 5.19056503815e-07),
    # NAD83 - NAD83 / Texas State Mapping System
    (3081, -109.575361, 21.771660, 1e-4, 0.0, 0.0, 1.52445863932e-07),
    # NAD83 - NAD83 / Texas Centric Lambert Conformal
    (3082, -109.731302, -20.457909, 1e-4, 0.0, 0.0, 2.32248567045e-07),
    # NAD83 - NAD83 / Texas Centric Albers Equal Area
    (3083, -109.212659, -58.634299, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas Centric Lambert Conformal
    (3084, -109.731302, -20.457909, 1e-4, 0.0, 0.0, 2.32248567045e-07),
    # NAD83(HARN) - NAD83(HARN) / Texas Centric Albers Equal Area
    (3085, -109.212659, -58.634299, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Florida GDL Albers
    (3086, -87.929804, 23.942448, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Florida GDL Albers
    (3087, -87.929804, 23.942448, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Kentucky Single Zone
    (3088, -100.546842, 26.314343, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Kentucky Single Zone (ftUS)
    (3089, -100.546842, 26.314343, 1e-4, 0.0, 0.0, 4.10775683122e-07),
    # NAD83(HARN) - NAD83(HARN) / Kentucky Single Zone
    (3090, -100.546842, 26.314343, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Kentucky Single Zone (ftUS)
    (3091, -100.546842, 26.314343, 1e-4, 0.0, 0.0, 4.10775683122e-07),
    # Tokyo - Tokyo / UTM zone 51N
    (3092, 118.509717, 0.006155, 1e-4, 0.0, 0.0, 0.000167022604899),
    # Tokyo - Tokyo / UTM zone 52N
    (3093, 124.509238, 0.006155, 1e-4, 0.0, 0.0, 0.000167022604728),
    # Tokyo - Tokyo / UTM zone 53N
    (3094, 130.508776, 0.006155, 1e-4, 0.0, 0.0, 0.00016701981076),
    # Tokyo - Tokyo / UTM zone 54N
    (3095, 136.508335, 0.006155, 1e-4, 0.0, 0.0, 0.000167022604728),
    # Tokyo - Tokyo / UTM zone 55N
    (3096, 142.507920, 0.006155, 1e-4, 0.0, 0.0, 0.000167022604899),
    # JGD2000 - JGD2000 / UTM zone 51N
    (3097, 118.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # JGD2000 - JGD2000 / UTM zone 52N
    (3098, 124.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # JGD2000 - JGD2000 / UTM zone 53N
    (3099, 130.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # JGD2000 - JGD2000 / UTM zone 54N
    (3100, 136.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167725724168),
    # JGD2000 - JGD2000 / UTM zone 55N
    (3101, 142.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # American Samoa 1962 - American Samoa 1962 / American Samoa Lambert
    (3102, -171.418900, -15.117587, 1e-4, 0.0, 0.0, 5.45838956896e-07),
    # Mauritania 1999 - Mauritania 1999 / UTM zone 28N (deprecated)
    (3103, -19.488665, 0.0, 1e-4, 0.0, 0.0, 0.000172127009137),
    # Mauritania 1999 - Mauritania 1999 / UTM zone 29N (deprecated)
    (3104, -13.488665, 0.0, 1e-4, 0.0, 0.0, 0.000172127009137),
    # Mauritania 1999 - Mauritania 1999 / UTM zone 30N (deprecated)
    (3105, -7.488665, 0.0, 1e-4, 0.0, 0.0, 0.000172127009137),
    # Gulshan 303 - Gulshan 303 / Bangladesh Transverse Mercator
    (3106, 85.508628, 0.002361, 1e-4, 0.0, 0.0, 0.000165573495991),
    # GDA94 - GDA94 / SA Lambert
    (3107, 121.863980, -49.226607, 1e-4, 0.0, 0.0, 2.50426819548e-06),
    # ETRS89 - ETRS89 / Guernsey Grid
    (3108, -3.059621, 49.048628, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / Jersey Transverse Mercator
    (3109, -2.677269, 48.594272, 1e-4, 0.0, 0.0, 1e-07),
    # AGD66 - AGD66 / Vicgrid66
    (3110, 91.530780, -68.875320, 1e-4, 0.0, 0.0, 8.9670997113e-05),
    # GDA94 - GDA94 / Vicgrid94
    (3111, 107.261088, -54.671212, 1e-4, 0.0, 0.0, 1.06869265437e-06),
    # GDA94 - GDA94 / Geoscience Australia Lambert
    (3112, 134.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # GDA94 - GDA94 / BCSG02
    (3113, 152.487343, -28.901343, 1e-4, 0.0, 0.0, 1e-07),
    # MAGNA-SIRGAS - MAGNA-SIRGAS / Colombia Far West zone
    (3114, -89.050237, -4.392732, 1e-4, 0.0, 0.0, 0.0141302149859),
    # MAGNA-SIRGAS - MAGNA-SIRGAS / Colombia West zone
    (3115, -86.050237, -4.392732, 1e-4, 0.0, 0.0, 0.0141302170814),
    # MAGNA-SIRGAS - MAGNA-SIRGAS / Colombia Bogota zone
    (3116, -83.050237, -4.392732, 1e-4, 0.0, 0.0, 0.0141302149859),
    # MAGNA-SIRGAS - MAGNA-SIRGAS / Colombia East Central zone
    (3117, -80.050237, -4.392732, 1e-4, 0.0, 0.0, 0.0141302149859),
    # MAGNA-SIRGAS - MAGNA-SIRGAS / Colombia East zone
    (3118, -77.050237, -4.392732, 1e-4, 0.0, 0.0, 0.0141302149859),
    # Douala 1948 - Douala 1948 / AEF west
    (3119, 1.433278, -8.940758, 1e-4, 0.0, 0.0, 0.0126349318307),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Poland zone I
    (3120, -12.432833, -0.869936, 1e-4, 0.0, 0.0, 0.000260935630649),
    # PRS92 - PRS92 / Philippines zone 1
    (3121, 112.514602, -0.000693, 1e-4, 0.0, 0.0, 0.00385681715921),
    # PRS92 - PRS92 / Philippines zone 2
    (3122, 114.514605, -0.000637, 1e-4, 0.0, 0.0, 0.00353874883736),
    # PRS92 - PRS92 / Philippines zone 3
    (3123, 116.514607, -0.000581, 1e-4, 0.0, 0.0, 0.00323111651752),
    # PRS92 - PRS92 / Philippines zone 4
    (3124, 118.514607, -0.000525, 1e-4, 0.0, 0.0, 0.00293545156391),
    # PRS92 - PRS92 / Philippines zone 5
    (3125, 120.514606, -0.000468, 1e-4, 0.0, 0.0, 0.00265321198517),
    # ETRS89 - ETRS89 / ETRS-GK19FIN
    (3126, 14.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # ETRS89 - ETRS89 / ETRS-GK20FIN
    (3127, 15.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # ETRS89 - ETRS89 / ETRS-GK21FIN
    (3128, 16.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # ETRS89 - ETRS89 / ETRS-GK22FIN
    (3129, 17.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # ETRS89 - ETRS89 / ETRS-GK23FIN
    (3130, 18.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # ETRS89 - ETRS89 / ETRS-GK24FIN
    (3131, 19.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # ETRS89 - ETRS89 / ETRS-GK25FIN
    (3132, 20.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405116372),
    # ETRS89 - ETRS89 / ETRS-GK26FIN
    (3133, 21.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # ETRS89 - ETRS89 / ETRS-GK27FIN
    (3134, 22.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # ETRS89 - ETRS89 / ETRS-GK28FIN
    (3135, 23.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405116372),
    # ETRS89 - ETRS89 / ETRS-GK29FIN
    (3136, 24.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # ETRS89 - ETRS89 / ETRS-GK30FIN
    (3137, 25.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405116372),
    # ETRS89 - ETRS89 / ETRS-GK31FIN
    (3138, 26.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # Viti Levu 1912 - Viti Levu 1912 / Viti Levu Grid
    (3140, 176.955190, -19.275048, 1e-4, 0.0, 0.0, 0.00151161187801),
    # Fiji 1956 - Fiji 1956 / UTM zone 60S
    (3141, 55.452449, -89.995816, 1e-4, 0.0, 0.0, 3),
    # Fiji 1956 - Fiji 1956 / UTM zone 1S
    (3142, 55.452449, -89.995816, 1e-4, 0.0, 0.0, 3),
    # Fiji 1986 - Fiji 1986 / Fiji Map Grid (deprecated)
    (3143, 150.870169, -49.599191, 1e-4, 0.0, 0.0, 1117),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 6 (deprecated)
    (3146, -32.069136, -0.000815, 1e-4, 0.0, 0.0, 66222),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger CM 18E (deprecated)
    (3147, 13.512067, -0.000827, 1e-4, 0.0, 0.0, 0.000353454844562),
    # Indian 1960 - Indian 1960 / UTM zone 48N
    (3148, 100.507459, 0.002867, 1e-4, 0.0, 0.0, 0.000165569828823),
    # Indian 1960 - Indian 1960 / UTM zone 49N
    (3149, 106.506697, 0.002867, 1e-4, 0.0, 0.0, 0.000165569828823),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 6 (deprecated)
    (3150, -32.069246, -0.000738, 1e-4, 0.0, 0.0, 66222),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger CM 18E (deprecated)
    (3151, 13.511965, -0.000738, 1e-4, 0.0, 0.0, 0.000171152874849),
    # SWEREF99 - ST74
    (3152, 16.334071, 58.611251, 1e-4, 0.0, 0.0, 4.27346094511e-07),
    # NAD83(CSRS) - NAD83(CSRS) / BC Albers
    (3153, -138.445861, 44.199437, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 7N
    (3154, -145.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 8N
    (3155, -139.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 9N
    (3156, -133.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 10N
    (3157, -127.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 14N
    (3158, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 15N
    (3159, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 16N
    (3160, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / Ontario MNR Lambert
    (3161, -88.910803, -31.975819, 1e-4, 0.0, 0.0, 1.97498593479e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Ontario MNR Lambert
    (3162, -88.910803, -31.975819, 1e-4, 0.0, 0.0, 1.97498593479e-07),
    # RGNC91-93 - RGNC91-93 / Lambert New Caledonia
    (3163, 162.067316, -24.162891, 1e-4, 0.0, 0.0, 1e-07),
    # ST87 Ouvea - ST87 Ouvea / UTM zone 58S
    (3164, 163.997324, -89.999476, 1e-4, 0.0, 0.0, 3),
    # NEA74 Noumea - NEA74 Noumea / Noumea Lambert
    (3165, 166.445748, -22.266936, 1e-4, 0.0, 0.0, 1e-07),
    # NEA74 Noumea - NEA74 Noumea / Noumea Lambert 2
    (3166, 166.445748, -22.266936, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau (RSO) - Kertau (RSO) / RSO Malaya (ch)
    (3167, 104.877013, -0.000173, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau (RSO) - Kertau (RSO) / RSO Malaya (m)
    (3168, 98.018200, -0.009462, 1e-4, 0.0, 0.0, 3.0765137597e-06),
    # RGNC91-93 - RGNC91-93 / UTM zone 57S
    (3169, 159.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGNC91-93 - RGNC91-93 / UTM zone 58S
    (3170, 165.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGNC91-93 - RGNC91-93 / UTM zone 59S
    (3171, 171.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # IGN53 Mare - IGN53 Mare / UTM zone 59S
    (3172, 31.724075, -89.996973, 1e-4, 0.0, 0.0, 3),
    # NAD83 - NAD83 / Great Lakes Albers
    (3174, -95.453299, 35.922697, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Great Lakes and St Lawrence Albers
    (3175, -94.245971, 35.922697, 1e-4, 0.0, 0.0, 1e-07),
    # Indian 1960 - Indian 1960 / TM 106 NE
    (3176, 101.507329, 0.002867, 1e-4, 0.0, 0.0, 0.000165572099007),
    # LGD2006 - LGD2006 / Libya TM
    (3177, 8.022144, -0.000023, 1e-4, 0.0, 0.0, 0.0144585419912),
    # GR96 - GR96 / UTM zone 18N
    (3178, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # GR96 - GR96 / UTM zone 19N
    (3179, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # GR96 - GR96 / UTM zone 20N
    (3180, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # GR96 - GR96 / UTM zone 21N
    (3181, -61.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # GR96 - GR96 / UTM zone 22N
    (3182, -55.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # GR96 - GR96 / UTM zone 23N
    (3183, -49.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # GR96 - GR96 / UTM zone 24N
    (3184, -43.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # GR96 - GR96 / UTM zone 25N
    (3185, -37.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # GR96 - GR96 / UTM zone 26N
    (3186, -31.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # GR96 - GR96 / UTM zone 27N
    (3187, -25.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # GR96 - GR96 / UTM zone 28N
    (3188, -19.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # GR96 - GR96 / UTM zone 29N
    (3189, -13.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # LGD2006 - LGD2006 / Libya TM zone 5
    (3190, 7.202902, -0.000023, 1e-4, 0.0, 0.0, 1.19734613691e-06),
    # LGD2006 - LGD2006 / Libya TM zone 6
    (3191, 9.202972, -0.000023, 1e-4, 0.0, 0.0, 1.19656033348e-06),
    # LGD2006 - LGD2006 / Libya TM zone 7
    (3192, 11.203042, -0.000023, 1e-4, 0.0, 0.0, 1.1970842031e-06),
    # LGD2006 - LGD2006 / Libya TM zone 8
    (3193, 13.203113, -0.000023, 1e-4, 0.0, 0.0, 1.19821925182e-06),
    # LGD2006 - LGD2006 / Libya TM zone 9
    (3194, 15.203185, -0.000023, 1e-4, 0.0, 0.0, 1.19900505591e-06),
    # LGD2006 - LGD2006 / Libya TM zone 10
    (3195, 17.203257, -0.000023, 1e-4, 0.0, 0.0, 1.19760807138e-06),
    # LGD2006 - LGD2006 / Libya TM zone 11
    (3196, 19.203330, -0.000023, 1e-4, 0.0, 0.0, 1.19708420243e-06),
    # LGD2006 - LGD2006 / Libya TM zone 12
    (3197, 21.203404, -0.000023, 1e-4, 0.0, 0.0, 1.19621108818e-06),
    # LGD2006 - LGD2006 / Libya TM zone 13
    (3198, 23.203477, -0.000023, 1e-4, 0.0, 0.0, 1.19848118696e-06),
    # LGD2006 - LGD2006 / UTM zone 32N
    (3199, 4.510596, -0.000023, 1e-4, 0.0, 0.0, 0.000168836500962),
    # FD58 - FD58 / Iraq zone
    (3200, 30.772169, 21.130869, 1e-4, 0.0, 0.0, 1.85100361705e-07),
    # LGD2006 - LGD2006 / UTM zone 33N
    (3201, 10.510804, -0.000023, 1e-4, 0.0, 0.0, 0.000168836675585),
    # LGD2006 - LGD2006 / UTM zone 34N
    (3202, 16.511018, -0.000023, 1e-4, 0.0, 0.0, 0.000168837548699),
    # LGD2006 - LGD2006 / UTM zone 35N
    (3203, 22.511237, -0.000023, 1e-4, 0.0, 0.0, 0.000168836675584),
    # WGS 84 - WGS 84 / SCAR IMW SP19-20
    (3204, -66.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SP21-22
    (3205, -54.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SP23-24
    (3206, -42.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ01-02
    (3207, -174.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ19-20
    (3208, -66.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ21-22
    (3209, -54.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ37-38
    (3210, 42.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ39-40
    (3211, 54.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ41-42
    (3212, 66.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ43-44
    (3213, 78.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ45-46
    (3214, 90.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ47-48
    (3215, 102.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ49-50
    (3216, 114.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ51-52
    (3217, 126.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ53-54
    (3218, 138.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ55-56
    (3219, 150.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SQ57-58
    (3220, 162.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR13-14
    (3221, -102.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR15-16
    (3222, -90.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR17-18
    (3223, -78.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR19-20
    (3224, -66.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR27-28
    (3225, -18.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR29-30
    (3226, -6.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR31-32
    (3227, 6.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR33-34
    (3228, 18.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR35-36
    (3229, 30.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR37-38
    (3230, 42.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR39-40
    (3231, 54.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR41-42
    (3232, 66.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR43-44
    (3233, 78.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR45-46
    (3234, 90.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR47-48
    (3235, 102.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR49-50
    (3236, 114.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR51-52
    (3237, 126.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR53-54
    (3238, 138.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR55-56
    (3239, 150.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR57-58
    (3240, 162.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SR59-60
    (3241, 174.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS04-06
    (3242, -153.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS07-09
    (3243, -135.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS10-12
    (3244, -117.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS13-15
    (3245, -99.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS16-18
    (3246, -81.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS19-21
    (3247, -63.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS25-27
    (3248, -27.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS28-30
    (3249, -9.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS31-33
    (3250, 9.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS34-36
    (3251, 27.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS37-39
    (3252, 45.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS40-42
    (3253, 63.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS43-45
    (3254, 81.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS46-48
    (3255, 99.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS49-51
    (3256, 117.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS52-54
    (3257, 135.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS55-57
    (3258, 153.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SS58-60
    (3259, 171.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST01-04
    (3260, -168.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST05-08
    (3261, -144.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST09-12
    (3262, -120.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST13-16
    (3263, -96.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST17-20
    (3264, -72.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST21-24
    (3265, -48.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST25-28
    (3266, -24.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST29-32
    (3267, 0.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST33-36
    (3268, 24.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST37-40
    (3269, 48.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST41-44
    (3270, 72.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST45-48
    (3271, 96.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST49-52
    (3272, 120.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST53-56
    (3273, 144.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW ST57-60
    (3274, 168.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU01-05
    (3275, -165.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU06-10
    (3276, -135.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU11-15
    (3277, -105.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU16-20
    (3278, -75.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU21-25
    (3279, -45.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU26-30
    (3280, -15.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU31-35
    (3281, 15.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU36-40
    (3282, 45.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU41-45
    (3283, 75.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU46-50
    (3284, 105.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU51-55
    (3285, 135.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SU56-60
    (3286, 165.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SV01-10
    (3287, -150.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SV11-20
    (3288, -90.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SV21-30
    (3289, -30.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SV31-40
    (3290, 30.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SV41-50
    (3291, 90.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SV51-60
    (3292, 150.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / SCAR IMW SW01-60
    (3293, 0.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / USGS Transantarctic Mountains
    (3294, 162.0, -78.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGPF - RGPF / UTM zone 5S
    (3296, -86.646557, -89.999990, 1e-4, 0.0, 0.0, 3),
    # RGPF - RGPF / UTM zone 6S
    (3297, -86.646557, -89.999990, 1e-4, 0.0, 0.0, 3),
    # RGPF - RGPF / UTM zone 7S
    (3298, -86.646557, -89.999990, 1e-4, 0.0, 0.0, 3),
    # RGPF - RGPF / UTM zone 8S
    (3299, -86.646557, -89.999990, 1e-4, 0.0, 0.0, 3),
    # EST92 - Estonian Coordinate System of 1992
    (3300, 20.776122, 6.570972, 1e-4, 0.0, 0.0, 1e-07),
    # EST97 - Estonian Coordinate System of 1997
    (3301, 20.776129, 6.570972, 1e-4, 0.0, 0.0, 1e-07),
    # IGN63 Hiva Oa - IGN63 Hiva Oa / UTM zone 7S
    (3302, 21.684280, -89.996741, 1e-4, 0.0, 0.0, 4),
    # Fatu Iva 72 - Fatu Iva 72 / UTM zone 7S
    (3303, 178.950699, -89.983596, 2e-4, 0.0, 0.0, 752571),
    # Tahiti 79 - Tahiti 79 / UTM zone 6S
    (3304, 16.728493, -89.997529, 1e-4, 0.0, 0.0, 4),
    # Moorea 87 - Moorea 87 / UTM zone 6S
    (3305, 10.383542, -89.997564, 1e-4, 0.0, 0.0, 4),
    # Maupiti 83 - Maupiti 83 / UTM zone 5S
    (3306, 21.834219, -89.997907, 1e-4, 0.0, 0.0, 3),
    # Nakhl-e Ghanem - Nakhl-e Ghanem / UTM zone 39N
    (3307, 46.511255, 0.000006, 1e-4, 0.0, 0.0, 0.000167730177054),
    # GDA94 - GDA94 / NSW Lambert
    (3308, 36.386613, -24.774135, 1e-4, 0.0, 0.0, 1.71829015017e-07),
    # NAD27 - NAD27 / California Albers
    (3309, -120.0, 38.018006, 2e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / California Albers
    (3310, -120.0, 38.016365, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / California Albers
    (3311, -120.0, 38.016365, 1e-4, 0.0, 0.0, 1e-07),
    # CSG67 - CSG67 / UTM zone 21N
    (3312, -61.489049, 0.000995, 1e-4, 0.0, 0.0, 0.000168835278622),
    # RGFG95 - RGFG95 / UTM zone 21N
    (3313, -61.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Katanga 1955 - Katanga 1955 / Katanga Lambert (deprecated)
    (3314, 26.000331, -0.002315, 1e-4, 0.0, 0.0, 1e-07),
    # Katanga 1955 - Katanga 1955 / Katanga TM (deprecated)
    (3315, 26.000335, -9.001761, 1e-4, 0.0, 0.0, 1e-07),
    # Kasai 1953 - Kasai 1953 / Congo TM zone 22
    (3316, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # Kasai 1953 - Kasai 1953 / Congo TM zone 24
    (3317, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # IGC 1962 6th Parallel South - IGC 1962 / Congo TM zone 12
    (3318, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # IGC 1962 6th Parallel South - IGC 1962 / Congo TM zone 14
    (3319, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # IGC 1962 6th Parallel South - IGC 1962 / Congo TM zone 16
    (3320, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # IGC 1962 6th Parallel South - IGC 1962 / Congo TM zone 18
    (3321, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # IGC 1962 6th Parallel South - IGC 1962 / Congo TM zone 20
    (3322, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # IGC 1962 6th Parallel South - IGC 1962 / Congo TM zone 22
    (3323, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # IGC 1962 6th Parallel South - IGC 1962 / Congo TM zone 24
    (3324, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # IGC 1962 6th Parallel South - IGC 1962 / Congo TM zone 26
    (3325, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # IGC 1962 6th Parallel South - IGC 1962 / Congo TM zone 28
    (3326, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # IGC 1962 6th Parallel South - IGC 1962 / Congo TM zone 30
    (3327, -180.0, -3694423501049548800.0, 1e-4, 0.0, 0.0, 9.67197810846e+16),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / GUGiK-80
    (3328, 12.535805, 47.473114, 1e-4, 0.0, 0.0, 0.000236975873122),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 5
    (3329, -29.144697, -0.000628, 1e-4, 0.0, 0.0, 15649),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 6
    (3330, -32.069125, -0.000624, 1e-4, 0.0, 0.0, 66222),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 7
    (3331, -33.997540, -0.000622, 1e-4, 0.0, 0.0, 228895),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 8
    (3332, -34.542175, -0.000621, 1e-4, 0.0, 0.0, 675426),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Gauss-Kruger zone 3
    (3333, -14.962310, -0.000650, 1e-4, 0.0, 0.0, 325),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Gauss-Kruger zone 4
    (3334, -16.408649, -0.000647, 1e-4, 0.0, 0.0, 2788),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Gauss-Kruger zone 5
    (3335, -17.144863, -0.000646, 1e-4, 0.0, 0.0, 15649),
    # IGN 1962 Kerguelen - IGN 1962 Kerguelen / UTM zone 42S
    (3336, -52.209962, -89.997881, 1e-4, 0.0, 0.0, 752569),
    # Le Pouce 1934 - Le Pouce 1934 / Mauritius Grid
    (3337, 47.388566, -28.916541, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Alaska Albers
    (3338, -154.0, 50.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGCB 1955 - IGCB 1955 / UTM zone 33S
    (3342, -116.825551, -89.998415, 1e-4, 0.0, 0.0, 753200),
    # Mauritania 1999 - Mauritania 1999 / UTM zone 28N
    (3343, -19.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # Mauritania 1999 - Mauritania 1999 / UTM zone 29N
    (3344, -13.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # Mauritania 1999 - Mauritania 1999 / UTM zone 30N
    (3345, -7.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # LKS94 - LKS94 / Lithuania TM
    (3346, 19.512152, 0.0, 1e-4, 0.0, 0.0, 0.000167567079188),
    # NAD83 - NAD83 / Statistics Canada Lambert
    (3347, -142.427811, 16.276099, 1e-4, 0.0, 0.0, 2.79396772385e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Statistics Canada Lambert
    (3348, -142.427811, 16.276099, 1e-4, 0.0, 0.0, 2.79396772385e-07),
    # WGS 84 - WGS 84 / PDC Mercator (deprecated)
    (3349, -150.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Pulkovo 1942 - Pulkovo 1942 / CS63 zone C0
    (3350, 19.703786, 0.099099, 1e-4, 0.0, 0.0, 0.00020266328165),
    # Pulkovo 1942 - Pulkovo 1942 / CS63 zone C1
    (3351, 13.791851, 0.097271, 1e-4, 0.0, 0.0, 0.0720523351279),
    # Pulkovo 1942 - Pulkovo 1942 / CS63 zone C2
    (3352, 8.146506, 0.093217, 1e-4, 0.0, 0.0, 8),
    # Mhast (onshore) - Mhast (onshore) / UTM zone 32S
    (3353, 9.0, -90.0, 1e-4, 0.0, 0.0, 752569),
    # Mhast (offshore) - Mhast (offshore) / UTM zone 32S
    (3354, 9.0, -90.0, 1e-4, 0.0, 0.0, 752569),
    # Egypt Gulf of Suez S-650 TL - Egypt Gulf of Suez S-650 TL / Red Belt
    (3355, 25.029096, 22.578165, 1e-4, 0.0, 0.0, 0.000327133631799),
    # GCGD59 - Grand Cayman 1959 / UTM zone 17N (deprecated)
    (3356, -85.488673, 0.001763, 1e-4, 0.0, 0.0, 0.0458169840163),
    # SIGD61 - Little Cayman 1961 / UTM zone 17N (deprecated)
    (3357, -85.487830, 0.001791, 1e-4, 0.0, 0.0, 0.00403616414435),
    # NAD83(HARN) - NAD83(HARN) / North Carolina
    (3358, -85.568291, 33.568155, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / North Carolina (ftUS) (deprecated)
    (3359, -85.568291, 33.568155, 1e-4, 0.0, 0.0, 2.26371230378e-07),
    # NAD83(HARN) - NAD83(HARN) / South Carolina
    (3360, -87.429394, 31.662328, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / South Carolina (ft)
    (3361, -87.429394, 31.662328, 1e-4, 0.0, 0.0, 2.6761616217e-07),
    # NAD83(HARN) - NAD83(HARN) / Pennsylvania North
    (3362, -84.776606, 39.947400, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Pennsylvania North (ftUS)
    (3363, -84.776606, 39.947400, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Pennsylvania South
    (3364, -84.693691, 39.120795, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Pennsylvania South (ftUS)
    (3365, -84.693691, 39.120795, 1e-4, 0.0, 0.0, 1e-07),
    # Hong Kong 1963 - Hong Kong 1963 Grid System (deprecated)
    (3366, 113.788462, 22.139444, 1e-4, 0.0, 0.0, 4.29585088568e-06),
    # IGN Astro 1960 - IGN Astro 1960 / UTM zone 28N
    (3367, -19.488665, 0.0, 1e-4, 0.0, 0.0, 0.000172127009137),
    # IGN Astro 1960 - IGN Astro 1960 / UTM zone 29N
    (3368, -13.488665, 0.0, 1e-4, 0.0, 0.0, 0.000172127009137),
    # IGN Astro 1960 - IGN Astro 1960 / UTM zone 30N
    (3369, -7.488665, 0.0, 1e-4, 0.0, 0.0, 0.000172127009137),
    # NAD27 - NAD27 / UTM zone 59N
    (3370, 166.511305, 0.0, 1e-4, 0.0, 0.0, 0.00017071221373),
    # NAD27 - NAD27 / UTM zone 60N
    (3371, 172.511305, 0.0, 1e-4, 0.0, 0.0, 0.000170716491994),
    # NAD83 - NAD83 / UTM zone 59N
    (3372, 166.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 60N
    (3373, 172.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # FD54 - FD54 / UTM zone 29N
    (3374, -13.488567, 0.0, 1e-4, 0.0, 0.0, 0.0001688352786),
    # GDM2000 - GDM2000 / Peninsula RSO
    (3375, 98.018974, -0.009460, 1e-4, 0.0, 0.0, 3.17871340956e-06),
    # GDM2000 - GDM2000 / East Malaysia BRSO
    (3376, 109.685821, -0.000173, 1e-4, 0.0, 0.0, 1e-07),
    # GDM2000 - GDM2000 / Johor Grid
    (3377, 103.561066, 2.042468, 1e-4, 0.0, 0.0, 1e-07),
    # GDM2000 - GDM2000 / Sembilan and Melaka Grid
    (3378, 101.941866, 2.720697, 1e-4, 0.0, 0.0, 1e-07),
    # GDM2000 - GDM2000 / Pahang Grid
    (3379, 102.434627, 3.710732, 1e-4, 0.0, 0.0, 1e-07),
    # GDM2000 - GDM2000 / Selangor Grid
    (3380, 101.702524, 3.173976, 1e-4, 0.0, 0.0, 1e-07),
    # GDM2000 - GDM2000 / Terengganu Grid
    (3381, 102.893604, 4.945770, 1e-4, 0.0, 0.0, 1e-07),
    # GDM2000 - GDM2000 / Pinang Grid
    (3382, 100.344588, 5.420954, 1e-4, 0.0, 0.0, 1e-07),
    # GDM2000 - GDM2000 / Kedah and Perlis Grid
    (3383, 100.636371, 5.964673, 1e-4, 0.0, 0.0, 1e-07),
    # GDM2000 - GDM2000 / Perak Grid
    (3384, 100.815427, 3.652206, 1e-4, 0.0, 0.0, 1e-07),
    # GDM2000 - GDM2000 / Kelantan Grid
    (3385, 102.175787, 5.893498, 1e-4, 0.0, 0.0, 1e-07),
    # KKJ - KKJ / Finland zone 0
    (3386, 13.512324, -0.000881, 1e-4, 0.0, 0.0, 0.00304971937592),
    # KKJ - KKJ / Finland zone 5
    (3387, -11.144279, -0.001455, 1e-4, 0.0, 0.0, 15649),
    # Pulkovo 1942 - Pulkovo 1942 / Caspian Sea Mercator
    (3388, 50.999262, -0.000793, 1e-4, 0.0, 0.0, 0.000209472918714),
    # Pulkovo 1942 - Pulkovo 1942 / 3-degree Gauss-Kruger zone 60
    (3389, 14.598359, -0.000826, 1e-4, 0.0, 0.0, 0.382388740452),
    # Pulkovo 1995 - Pulkovo 1995 / 3-degree Gauss-Kruger zone 60
    (3390, 14.598256, -0.000738, 1e-4, 0.0, 0.0, 0.382215540994),
    # Karbala 1979 - Karbala 1979 / UTM zone 37N
    (3391, 34.508488, 0.002378, 1e-4, 0.0, 0.0, 0.000172126485268),
    # Karbala 1979 - Karbala 1979 / UTM zone 38N
    (3392, 40.508627, 0.002378, 1e-4, 0.0, 0.0, 0.000172129279321),
    # Karbala 1979 - Karbala 1979 / UTM zone 39N
    (3393, 46.508796, 0.002378, 1e-4, 0.0, 0.0, 0.000172128231583),
    # Nahrwan 1934 - Nahrwan 1934 / Iraq zone
    (3394, 30.772328, 21.130623, 2e-3, 0.0, 0.0, 1.87195837498e-07),
    # WGS 84 - WGS 84 / World Mercator
    (3395, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # PD/83 - PD/83 / 3-degree Gauss-Kruger zone 3
    (3396, -20.964993, 0.0, 1e-4, 0.0, 0.0, 325),
    # PD/83 - PD/83 / 3-degree Gauss-Kruger zone 4
    (3397, -25.412011, 0.0, 1e-4, 0.0, 0.0, 2790),
    # RD/83 - RD/83 / 3-degree Gauss-Kruger zone 4
    (3398, -25.412011, 0.0, 1e-4, 0.0, 0.0, 2790),
    # RD/83 - RD/83 / 3-degree Gauss-Kruger zone 5
    (3399, -29.148663, 0.0, 1e-4, 0.0, 0.0, 15664),
    # NAD83 - NAD83 / Alberta 10-TM (Forest)
    (3400, -119.490537, 0.0, 1e-4, 0.0, 0.0, 0.000168049911736),
    # NAD83 - NAD83 / Alberta 10-TM (Resource)
    (3401, -115.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Alberta 10-TM (Forest)
    (3402, -119.490537, 0.0, 1e-4, 0.0, 0.0, 0.000168049911736),
    # NAD83(CSRS) - NAD83(CSRS) / Alberta 10-TM (Resource)
    (3403, -115.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / North Carolina (ftUS)
    (3404, -85.568291, 33.568155, 1e-4, 0.0, 0.0, 2.26943686538e-07),
    # VN-2000 - VN-2000 / UTM zone 48N
    (3405, 100.513017, -0.001006, 1e-4, 0.0, 0.0, 0.000167729571999),
    # VN-2000 - VN-2000 / UTM zone 49N
    (3406, 106.513010, -0.001007, 1e-4, 0.0, 0.0, 0.000167741280549),
    # Hong Kong 1963 - Hong Kong 1963 Grid System
    (3407, 113.788462, 22.139444, 1e-4, 0.0, 0.0, 1.40941254182e-05),
    # Unspecified International 1924 Authalic Sphere - NSIDC EASE-Grid North
    (3408, 180.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unspecified International 1924 Authalic Sphere - NSIDC EASE-Grid South
    (3409, 0.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unspecified International 1924 Authalic Sphere - NSIDC EASE-Grid Global
    (3410, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unspecified Hughes 1980 ellipsoid - NSIDC Sea Ice Polar Stereographic N
    (3411, -45.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unspecified Hughes 1980 ellipsoid - NSIDC Sea Ice Polar Stereographic S
    (3412, 0.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / NSIDC Sea Ice Polar Stereographic North
    (3413, -45.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # SVY21 - SVY21 / Singapore TM
    (3414, 103.581752, 1.016264, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 72BE - WGS 72BE / South China Sea Lambert
    (3415, 109.326806, 16.415415, 1e-4, 0.0, 0.0, 0.00017893416225),
    # ETRS89 - ETRS89 / Austria Lambert
    (3416, 8.369029, 43.787367, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Iowa North (ftUS)
    (3417, -109.055583, 31.312141, 1e-4, 0.0, 0.0, 3.24839347741e-07),
    # NAD83 - NAD83 / Iowa South (ftUS)
    (3418, -99.345436, 39.848662, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Kansas North (ftUS)
    (3419, -102.570641, 38.242381, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Kansas South (ftUS)
    (3420, -102.765768, 32.984182, 1e-4, 0.0, 0.0, 2.31455196626e-07),
    # NAD83 - NAD83 / Nevada East (ftUS)
    (3421, -117.845072, -37.495818, 1e-4, 0.0, 0.0, 8.85774454218e-06),
    # NAD83 - NAD83 / Nevada Central (ftUS)
    (3422, -121.423274, -19.408016, 1e-4, 0.0, 0.0, 0.000118735370052),
    # NAD83 - NAD83 / Nevada West (ftUS)
    (3423, -125.753742, -1.377653, 1e-4, 0.0, 0.0, 0.0101713249696),
    # NAD83 - NAD83 / New Jersey (ftUS)
    (3424, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 1.97644674377e-06),
    # NAD83(HARN) - NAD83(HARN) / Iowa North (ftUS)
    (3425, -109.055583, 31.312141, 1e-4, 0.0, 0.0, 3.24839347741e-07),
    # NAD83(HARN) - NAD83(HARN) / Iowa South (ftUS)
    (3426, -99.345436, 39.848662, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Kansas North (ftUS)
    (3427, -102.570641, 38.242381, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Kansas South (ftUS)
    (3428, -102.765768, 32.984182, 1e-4, 0.0, 0.0, 2.31455196626e-07),
    # NAD83(HARN) - NAD83(HARN) / Nevada East (ftUS)
    (3429, -117.845072, -37.495818, 1e-4, 0.0, 0.0, 8.85774454218e-06),
    # NAD83(HARN) - NAD83(HARN) / Nevada Central (ftUS)
    (3430, -121.423274, -19.408016, 1e-4, 0.0, 0.0, 0.000118735370052),
    # NAD83(HARN) - NAD83(HARN) / Nevada West (ftUS)
    (3431, -125.753742, -1.377653, 1e-4, 0.0, 0.0, 0.0101713249696),
    # NAD83(HARN) - NAD83(HARN) / New Jersey (ftUS)
    (3432, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 1.97644674377e-06),
    # NAD83 - NAD83 / Arkansas North (ftUS)
    (3433, -96.343203, 34.253806, 1e-4, 0.0, 0.0, 2.06034807436e-07),
    # NAD83 - NAD83 / Arkansas South (ftUS)
    (3434, -96.090762, 28.992599, 1e-4, 0.0, 0.0, 3.15959259751e-07),
    # NAD83 - NAD83 / Illinois East (ftUS)
    (3435, -91.686566, 36.619446, 1e-4, 0.0, 0.0, 8.26441060954e-05),
    # NAD83 - NAD83 / Illinois West (ftUS)
    (3436, -97.964340, 36.410947, 1e-4, 0.0, 0.0, 0.00838779730119),
    # NAD83 - NAD83 / New Hampshire (ftUS)
    (3437, -75.312874, 42.441965, 1e-4, 0.0, 0.0, 0.000140504705092),
    # NAD83 - NAD83 / Rhode Island (ftUS)
    (3438, -72.689948, 41.077189, 1e-4, 0.0, 0.0, 2.51308794467e-07),
    # PSD93 - PSD93 / UTM zone 39N
    (3439, 46.513434, 0.001774, 1e-4, 0.0, 0.0, 0.0195198468709),
    # PSD93 - PSD93 / UTM zone 40N
    (3440, 52.513706, 0.001716, 1e-4, 0.0, 0.0, 0.0195015741845),
    # NAD83(HARN) - NAD83(HARN) / Arkansas North (ftUS)
    (3441, -96.343203, 34.253806, 1e-4, 0.0, 0.0, 2.06034807436e-07),
    # NAD83(HARN) - NAD83(HARN) / Arkansas South (ftUS)
    (3442, -96.090762, 28.992599, 1e-4, 0.0, 0.0, 3.15959259751e-07),
    # NAD83(HARN) - NAD83(HARN) / Illinois East (ftUS)
    (3443, -91.686566, 36.619446, 1e-4, 0.0, 0.0, 8.26441060954e-05),
    # NAD83(HARN) - NAD83(HARN) / Illinois West (ftUS)
    (3444, -97.964340, 36.410947, 1e-4, 0.0, 0.0, 0.00838779730119),
    # NAD83(HARN) - NAD83(HARN) / New Hampshire (ftUS)
    (3445, -75.312874, 42.441965, 1e-4, 0.0, 0.0, 0.000140504705092),
    # NAD83(HARN) - NAD83(HARN) / Rhode Island (ftUS)
    (3446, -72.689948, 41.077189, 1e-4, 0.0, 0.0, 2.51308794467e-07),
    # ETRS89 - ETRS89 / Belgian Lambert 2005
    (3447, 2.293035, 49.284239, 1e-4, 0.0, 0.0, 2.18571221922e-06),
    # JAD2001 - JAD2001 / Jamaica Metric Grid
    (3448, -83.851787, 12.011121, 1e-4, 0.0, 0.0, 1e-07),
    # JAD2001 - JAD2001 / UTM zone 17N
    (3449, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # JAD2001 - JAD2001 / UTM zone 18N
    (3450, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # NAD83 - NAD83 / Louisiana North (ftUS)
    (3451, -102.882630, 30.067691, 1e-4, 0.0, 0.0, 3.17932299144e-07),
    # NAD83 - NAD83 / Louisiana South (ftUS)
    (3452, -101.517796, 28.098894, 1e-4, 0.0, 0.0, 3.50488795371e-07),
    # NAD83 - NAD83 / Louisiana Offshore (ftUS)
    (3453, -101.257010, 25.145011, 1e-4, 0.0, 0.0, 4.32216145998e-07),
    # NAD83 - NAD83 / South Dakota North (ftUS) (deprecated)
    (3454, -107.593704, 42.097136, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / South Dakota South (ftUS)
    (3455, -107.593704, 42.097136, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Louisiana North (ftUS)
    (3456, -102.882630, 30.067691, 1e-4, 0.0, 0.0, 3.17932299144e-07),
    # NAD83(HARN) - NAD83(HARN) / Louisiana South (ftUS)
    (3457, -101.517796, 28.098894, 1e-4, 0.0, 0.0, 3.50488795371e-07),
    # NAD83(HARN) - NAD83(HARN) / South Dakota North (ftUS)
    (3458, -107.437658, 43.585145, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / South Dakota South (ftUS)
    (3459, -107.593704, 42.097136, 1e-4, 0.0, 0.0, 1e-07),
    # Fiji 1986 - Fiji 1986 / Fiji Map Grid
    (3460, 150.870169, -49.599191, 1e-4, 0.0, 0.0, 1117),
    # Dabola 1981 - Dabola 1981 / UTM zone 28N
    (3461, -19.488600, 0.001121, 1e-4, 0.0, 0.0, 0.000172128493453),
    # Dabola 1981 - Dabola 1981 / UTM zone 29N
    (3462, -13.488515, 0.001121, 1e-4, 0.0, 0.0, 0.000172128493432),
    # NAD83 - NAD83 / Maine CS2000 Central
    (3463, -75.289767, 43.333516, 1e-4, 0.0, 0.0, 0.00085338508619),
    # NAD83(HARN) - NAD83(HARN) / Maine CS2000 Central
    (3464, -75.289767, 43.333516, 1e-4, 0.0, 0.0, 0.00085338508619),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alabama East
    (3465, -87.916194, 30.483360, 1e-4, 0.0, 0.0, 1.33309797832e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alabama West
    (3466, -93.703674, 29.853763, 1e-4, 0.0, 0.0, 0.00067773398073),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska Albers
    (3467, -154.0, 50.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska zone 1
    (3468, -145.375377, 51.236569, 1e-4, 0.0, 0.0, 1.76858156919e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska zone 2
    (3469, -149.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726326207989),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska zone 3
    (3470, -153.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska zone 4
    (3471, -157.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska zone 5
    (3472, -161.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska zone 6
    (3473, -165.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska zone 7
    (3474, -169.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726326207989),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska zone 8
    (3475, -173.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726326207989),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska zone 9
    (3476, -177.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Alaska zone 10
    (3477, 169.941515, 50.118842, 1e-4, 0.0, 0.0, 2.16796352387e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Arizona Central
    (3478, -114.150208, 30.980684, 1e-4, 0.0, 0.0, 2.03625043994e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Arizona Central (ft)
    (3479, -114.150208, 30.980684, 1e-4, 0.0, 0.0, 6.68061167961e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Arizona East
    (3480, -112.400208, 30.980684, 1e-4, 0.0, 0.0, 2.03809133589e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Arizona East (ft)
    (3481, -112.400208, 30.980684, 1e-4, 0.0, 0.0, 6.68665136447e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Arizona West
    (3482, -115.983467, 30.980685, 1e-4, 0.0, 0.0, 2.03845919723e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Arizona West (ft)
    (3483, -115.983467, 30.980685, 1e-4, 0.0, 0.0, 6.68785825863e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Arkansas North
    (3484, -96.343203, 34.253806, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Arkansas North (ftUS)
    (3485, -96.343203, 34.253806, 1e-4, 0.0, 0.0, 2.06034807436e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Arkansas South
    (3486, -96.090762, 28.992599, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Arkansas South (ftUS)
    (3487, -96.090762, 28.992599, 1e-4, 0.0, 0.0, 3.15959259751e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California Albers
    (3488, -120.0, 38.016365, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 1
    (3489, -143.321156, 32.649532, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 1 (ftUS)
    (3490, -143.321156, 32.649532, 1e-4, 0.0, 0.0, 2.79865998891e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 2
    (3491, -142.954019, 31.095993, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 2 (ftUS)
    (3492, -142.954019, 31.095993, 1e-4, 0.0, 0.0, 3.30854891217e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 3
    (3493, -141.218751, 30.009900, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 3 (ftUS)
    (3494, -141.218751, 30.009900, 1e-4, 0.0, 0.0, 3.35438162438e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 4
    (3495, -139.486472, 28.915572, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 4 (ftUS)
    (3496, -139.486472, 28.915572, 1e-4, 0.0, 0.0, 3.79552147933e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 5
    (3497, -138.152186, 27.196010, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 5 (ftUS)
    (3498, -138.152186, 27.196010, 1e-4, 0.0, 0.0, 4.19082862209e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 6
    (3499, -136.178771, 25.945490, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / California zone 6 (ftUS)
    (3500, -136.178771, 25.945490, 1e-4, 0.0, 0.0, 4.50306397397e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Colorado Central
    (3501, -115.464063, 34.638233, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Colorado Central (ftUS)
    (3502, -115.464063, 34.638233, 1e-4, 0.0, 0.0, 1.92210936802e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Colorado North
    (3503, -115.653598, 36.118325, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Colorado North (ftUS)
    (3504, -115.653598, 36.118325, 1e-4, 0.0, 0.0, 1.83330848813e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Colorado South
    (3505, -115.330280, 33.489028, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Colorado South (ftUS)
    (3506, -115.330280, 33.489028, 1e-4, 0.0, 0.0, 2.39189466811e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Connecticut
    (3507, -76.287472, 39.405061, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Connecticut (ftUS)
    (3508, -76.287472, 39.405061, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Delaware
    (3509, -77.692913, 37.977968, 1e-4, 0.0, 0.0, 2.82291068248e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Delaware (ftUS)
    (3510, -77.692913, 37.977968, 1e-4, 0.0, 0.0, 9.25010817428e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Florida East
    (3511, -82.970337, 24.320543, 1e-4, 0.0, 0.0, 4.93879810895e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Florida East (ftUS)
    (3512, -82.970337, 24.320543, 1e-4, 0.0, 0.0, 1.6175350876e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Florida GDL Albers
    (3513, -87.929804, 23.942448, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Florida North
    (3514, -90.650782, 28.853971, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Florida North (ftUS)
    (3515, -90.650782, 28.853971, 1e-4, 0.0, 0.0, 3.25091364585e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Florida West
    (3516, -83.970337, 24.320543, 1e-4, 0.0, 0.0, 4.93879810895e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Florida West (ftUS)
    (3517, -83.970337, 24.320543, 1e-4, 0.0, 0.0, 1.61132241679e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Georgia East
    (3518, -84.239141, 29.983687, 1e-4, 0.0, 0.0, 1.25848615201e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Georgia East (ftUS)
    (3519, -84.239141, 29.983687, 1e-4, 0.0, 0.0, 4.12373395515e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Georgia West
    (3520, -91.398144, 29.801257, 1e-4, 0.0, 0.0, 0.00142275930678),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Georgia West (ftUS)
    (3521, -91.398144, 29.801257, 1e-4, 0.0, 0.0, 0.00466783311378),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Idaho Central
    (3522, -119.988244, 41.510438, 1e-4, 0.0, 0.0, 0.000560564359692),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Idaho Central (ftUS)
    (3523, -119.988244, 41.510438, 1e-4, 0.0, 0.0, 0.00183911055059),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Idaho East
    (3524, -114.567265, 41.641591, 1e-4, 0.0, 0.0, 3.94830305492e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Idaho East (ftUS)
    (3525, -114.567265, 41.641591, 1e-4, 0.0, 0.0, 1.2961154679e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Idaho West
    (3526, -125.292410, 41.269025, 1e-4, 0.0, 0.0, 0.0284014806996),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Idaho West (ftUS)
    (3527, -125.292410, 41.269025, 1e-4, 0.0, 0.0, 0.0931805209225),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Illinois East
    (3528, -91.686566, 36.619446, 1e-4, 0.0, 0.0, 2.51901485408e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Illinois East (ftUS)
    (3529, -91.686566, 36.619446, 1e-4, 0.0, 0.0, 8.26441060954e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Illinois West
    (3530, -97.964340, 36.410947, 1e-4, 0.0, 0.0, 0.00255660697676),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Illinois West (ftUS)
    (3531, -97.964340, 36.410947, 1e-4, 0.0, 0.0, 0.00838779730119),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Indiana East
    (3532, -86.765357, 35.241995, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Indiana East (ftUS)
    (3533, -86.765357, 35.241995, 1e-4, 0.0, 0.0, 1.5225054085e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Indiana West
    (3534, -96.907562, 34.847248, 1e-4, 0.0, 0.0, 0.01940148008),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Indiana West (ftUS)
    (3535, -96.907562, 34.847248, 1e-4, 0.0, 0.0, 0.0636530351664),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Iowa North
    (3536, -109.055583, 31.312141, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Iowa North (ftUS)
    (3537, -109.055583, 31.312141, 1e-4, 0.0, 0.0, 3.24839347741e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Iowa South
    (3538, -99.345436, 39.848662, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Iowa South (ftUS)
    (3539, -99.345436, 39.848662, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Kansas North
    (3540, -102.570641, 38.242381, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Kansas North (ftUS)
    (3541, -102.570641, 38.242381, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Kansas South
    (3542, -102.765768, 32.984182, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Kansas South (ftUS)
    (3543, -102.765768, 32.984182, 1e-4, 0.0, 0.0, 2.31455196626e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Kentucky North
    (3544, -89.896811, 37.361875, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Kentucky North (ftUS)
    (3545, -89.896811, 37.361875, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Kentucky Single Zone
    (3546, -100.546842, 26.314343, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Kentucky Single Zone (ftUS)
    (3547, -100.546842, 26.314343, 1e-4, 0.0, 0.0, 4.10775683122e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Kentucky South
    (3548, -91.003100, 31.708902, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Kentucky South (ftUS)
    (3549, -91.003100, 31.708902, 1e-4, 0.0, 0.0, 2.81011816696e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Louisiana North
    (3550, -102.882630, 30.067691, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Louisiana North (ftUS)
    (3551, -102.882630, 30.067691, 1e-4, 0.0, 0.0, 3.17932299144e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Louisiana South
    (3552, -101.517796, 28.098894, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Louisiana South (ftUS)
    (3553, -101.517796, 28.098894, 1e-4, 0.0, 0.0, 3.50488795371e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maine CS2000 Central
    (3554, -75.289767, 43.333516, 1e-4, 0.0, 0.0, 0.00085338508619),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maine CS2000 East
    (3555, -76.529297, 43.504477, 1e-4, 0.0, 0.0, 0.0142061173473),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maine CS2000 West
    (3556, -74.040638, 42.774624, 1e-4, 0.0, 0.0, 4.41511791914e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maine East
    (3557, -72.216114, 43.606225, 1e-4, 0.0, 0.0, 4.76777649198e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maine West
    (3558, -81.077225, 42.310713, 1e-4, 0.0, 0.0, 0.102337038947),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maryland
    (3559, -81.529189, 37.577262, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Utah North (ftUS)
    (3560, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 2.83303452306e-07),
    # Old Hawaiian - Old Hawaiian / Hawaii zone 1
    (3561, -156.943115, 18.824707, 1e-4, 0.0, 0.0, 2.56925416956e-07),
    # Old Hawaiian - Old Hawaiian / Hawaii zone 2
    (3562, -158.123188, 20.324157, 1e-4, 0.0, 0.0, 1.63857680463e-07),
    # Old Hawaiian - Old Hawaiian / Hawaii zone 3
    (3563, -159.464474, 21.157196, 1e-4, 0.0, 0.0, 1e-07),
    # Old Hawaiian - Old Hawaiian / Hawaii zone 4
    (3564, -160.971149, 21.823634, 1e-4, 0.0, 0.0, 1e-07),
    # Old Hawaiian - Old Hawaiian / Hawaii zone 5
    (3565, -161.636102, 21.657042, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Utah Central (ftUS)
    (3566, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 5.27076190338e-07),
    # NAD83 - NAD83 / Utah South (ftUS)
    (3567, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 4.83248659293e-07),
    # NAD83(HARN) - NAD83(HARN) / Utah North (ftUS)
    (3568, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 2.83303452306e-07),
    # NAD83(HARN) - NAD83(HARN) / Utah Central (ftUS)
    (3569, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 5.27076190338e-07),
    # NAD83(HARN) - NAD83(HARN) / Utah South (ftUS)
    (3570, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 4.83248659293e-07),
    # WGS 84 - WGS 84 / North Pole LAEA Bering Sea
    (3571, 180.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / North Pole LAEA Alaska
    (3572, -150.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / North Pole LAEA Canada
    (3573, -100.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / North Pole LAEA Atlantic
    (3574, -40.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / North Pole LAEA Europe
    (3575, 10.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / North Pole LAEA Russia
    (3576, 90.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # GDA94 - GDA94 / Australian Albers
    (3577, 132.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Yukon Albers
    (3578, -140.092914, 54.207167, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Yukon Albers
    (3579, -140.092914, 54.207167, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / NWT Lambert
    (3580, -112.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / NWT Lambert
    (3581, -112.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maryland (ftUS)
    (3582, -81.529189, 37.577262, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Massachusetts Island
    (3583, -76.433406, 40.845825, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Massachusetts Island (ftUS)
    (3584, -76.433406, 40.845825, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Massachusetts Mainland
    (3585, -73.651391, 34.244387, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Massachusetts Mainland (ftUS)
    (3586, -73.651391, 34.244387, 1e-4, 0.0, 0.0, 2.03525887628e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Michigan Central
    (3587, -144.322231, 22.874000, 1e-4, 0.0, 0.0, 2.08924502698e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Michigan Central (ft)
    (3588, -144.322231, 22.874000, 1e-4, 0.0, 0.0, 6.80864563111e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Michigan North
    (3589, -158.790109, 11.663980, 1e-4, 0.0, 0.0, 2.29428967657e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Michigan North (ft)
    (3590, -158.790109, 11.663980, 1e-4, 0.0, 0.0, 7.44651609622e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Michigan Oblique Mercator
    (3591, -91.882286, 40.401569, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Michigan South
    (3592, -127.914179, 32.026364, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Michigan South (ft)
    (3593, -127.914179, 32.026364, 1e-4, 0.0, 0.0, 3.67591261454e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Minnesota Central
    (3594, -104.182085, 43.648811, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Minnesota North
    (3595, -103.287887, 45.126033, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Minnesota South
    (3596, -103.618869, 41.676372, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Mississippi East
    (3597, -91.925475, 29.464053, 1e-4, 0.0, 0.0, 1.2232144699e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Mississippi East (ftUS)
    (3598, -91.925475, 29.464053, 1e-4, 0.0, 0.0, 4.01310551576e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Mississippi West
    (3599, -97.529032, 29.305235, 1e-4, 0.0, 0.0, 0.00138477058454),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Mississippi West (ftUS)
    (3600, -97.529032, 29.305235, 1e-4, 0.0, 0.0, 0.00454319441734),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Missouri Central
    (3601, -98.022683, 35.706341, 1e-4, 0.0, 0.0, 0.000399243352018),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Missouri East
    (3602, -93.265663, 35.801506, 1e-4, 0.0, 0.0, 8.21736460543e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Missouri West
    (3603, -103.890580, 35.797519, 1e-4, 0.0, 0.0, 0.0140642820106),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Montana
    (3604, -116.985465, 43.991943, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Montana (ft)
    (3605, -116.985465, 43.991943, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Nebraska
    (3606, -105.831722, 39.681418, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Nevada Central
    (3607, -121.423274, -19.408016, 1e-4, 0.0, 0.0, 3.61900019925e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Nevada Central (ftUS)
    (3608, -121.423274, -19.408016, 1e-4, 0.0, 0.0, 0.000118735370052),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Nevada East
    (3609, -117.845072, -37.495818, 1e-4, 0.0, 0.0, 2.70277087111e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Nevada East (ftUS)
    (3610, -117.845072, -37.495818, 1e-4, 0.0, 0.0, 8.85774454218e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Nevada West
    (3611, -125.753742, -1.377653, 1e-4, 0.0, 0.0, 0.00310022570193),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Nevada West (ftUS)
    (3612, -125.753742, -1.377653, 1e-4, 0.0, 0.0, 0.0101713249696),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New Hampshire
    (3613, -75.312874, 42.441965, 1e-4, 0.0, 0.0, 4.28260943867e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New Hampshire (ftUS)
    (3614, -75.312874, 42.441965, 1e-4, 0.0, 0.0, 0.000140504705092),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New Jersey
    (3615, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 6.02422172344e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New Jersey (ftUS)
    (3616, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 1.97644674377e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New Mexico Central
    (3617, -111.476675, 30.894188, 1e-4, 0.0, 0.0, 0.000274516973348),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New Mexico Central (ftUS)
    (3618, -111.476675, 30.894188, 1e-4, 0.0, 0.0, 0.000900648487301),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New Mexico East
    (3619, -106.060830, 30.988445, 1e-4, 0.0, 0.0, 4.79974244135e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New Mexico East (ftUS)
    (3620, -106.060830, 30.988445, 1e-4, 0.0, 0.0, 1.5747154993e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New Mexico West
    (3621, -116.482847, 30.710010, 1e-4, 0.0, 0.0, 0.00241149640912),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New Mexico West (ftUS)
    (3622, -116.482847, 30.710010, 1e-4, 0.0, 0.0, 0.00791171227234),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New York Central
    (3623, -79.509326, 39.963054, 1e-4, 0.0, 0.0, 1.20960787919e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New York Central (ftUS)
    (3624, -79.509326, 39.963054, 1e-4, 0.0, 0.0, 3.96810172177e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New York East
    (3625, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 6.02422172344e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New York East (ftUS)
    (3626, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 1.97644674377e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New York Long Island
    (3627, -77.519584, 40.112385, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New York Long Island (ftUS)
    (3628, -77.519584, 40.112385, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New York West
    (3629, -82.677307, 39.927648, 1e-4, 0.0, 0.0, 8.10902805652e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / New York West (ftUS)
    (3630, -82.677307, 39.927648, 1e-4, 0.0, 0.0, 0.000266040889871),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / North Carolina
    (3631, -85.568291, 33.568155, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / North Carolina (ftUS)
    (3632, -85.568291, 33.568155, 1e-4, 0.0, 0.0, 2.26943686538e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / North Dakota North
    (3633, -108.360607, 46.724303, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / North Dakota North (ft)
    (3634, -108.360607, 46.724303, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / North Dakota South
    (3635, -108.173906, 45.402819, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / North Dakota South (ft)
    (3636, -108.173906, 45.402819, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Ohio North
    (3637, -89.475829, 39.450489, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Ohio South
    (3638, -89.316674, 37.795918, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Oklahoma North
    (3639, -104.561592, 34.817203, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Oklahoma North (ftUS)
    (3640, -104.561592, 34.817203, 1e-4, 0.0, 0.0, 1.91522645799e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Oklahoma South
    (3641, -104.434828, 33.160876, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Oklahoma South (ftUS)
    (3642, -104.434828, 33.160876, 1e-4, 0.0, 0.0, 2.41455848176e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Oregon LCC (m)
    (3643, -125.300329, 41.644767, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Oregon GIC Lambert (ft)
    (3644, -125.300329, 41.644767, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Oregon North
    (3645, -150.091081, 39.506127, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Oregon North (ft)
    (3646, -150.091081, 39.506127, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Oregon South
    (3647, -138.236892, 40.231913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Oregon South (ft)
    (3648, -138.236892, 40.231913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Pennsylvania North
    (3649, -84.776606, 39.947400, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Pennsylvania North (ftUS)
    (3650, -84.776606, 39.947400, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Pennsylvania South
    (3651, -84.693691, 39.120795, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Pennsylvania South (ftUS)
    (3652, -84.693691, 39.120795, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Rhode Island
    (3653, -72.689948, 41.077189, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Rhode Island (ftUS)
    (3654, -72.689948, 41.077189, 1e-4, 0.0, 0.0, 2.51308794467e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / South Carolina
    (3655, -87.429394, 31.662328, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / South Carolina (ft)
    (3656, -87.429394, 31.662328, 1e-4, 0.0, 0.0, 2.6761616217e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / South Dakota North
    (3657, -107.437658, 43.585145, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / South Dakota North (ftUS)
    (3658, -107.437658, 43.585145, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / South Dakota South
    (3659, -107.593704, 42.097136, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / South Dakota South (ftUS)
    (3660, -107.593704, 42.097136, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Tennessee
    (3661, -92.508665, 34.153466, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Tennessee (ftUS)
    (3662, -92.508665, 34.153466, 1e-4, 0.0, 0.0, 1.98492272166e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas Central
    (3663, -105.983204, 3.442337, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas Central (ftUS)
    (3664, -105.983204, 3.442337, 1e-4, 0.0, 0.0, 1.89059937838e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas Centric Albers Equal Area
    (3665, -109.212659, -58.634299, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas Centric Lambert Conformal
    (3666, -109.731302, -20.457909, 1e-4, 0.0, 0.0, 2.32248567045e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas North
    (3667, -103.450598, 25.015254, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas North (ftUS)
    (3668, -103.450598, 25.015254, 1e-4, 0.0, 0.0, 4.1378345486e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas North Central
    (3669, -103.763988, 13.819848, 1e-4, 0.0, 0.0, 1.59605406225e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas North Central (ftUS)
    (3670, -103.763988, 13.819848, 1e-4, 0.0, 0.0, 5.39107277291e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas South
    (3671, -100.642123, -15.495006, 1e-4, 0.0, 0.0, 2.08325218409e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas South (ftUS)
    (3672, -100.642123, -15.495006, 1e-4, 0.0, 0.0, 6.52829694445e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas South Central
    (3673, -103.518030, -6.145758, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Texas South Central (ftUS)
    (3674, -103.518030, -6.145758, 1e-4, 0.0, 0.0, 3.59786790796e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Utah Central
    (3675, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 1.61089701578e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Utah Central (ft)
    (3676, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 5.19629413863e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Utah Central (ftUS)
    (3677, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 5.27076190338e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Utah North
    (3678, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Utah North (ft)
    (3679, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 2.76429098334e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Utah North (ftUS)
    (3680, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 2.83303452306e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Utah South
    (3681, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 1.55065208673e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Utah South (ft)
    (3682, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 4.84108990865e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Utah South (ftUS)
    (3683, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 4.83248659293e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Vermont
    (3684, -78.566435, 42.339184, 1e-4, 0.0, 0.0, 0.000677358422587),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Virginia North
    (3685, -109.123093, 14.944862, 1e-4, 0.0, 0.0, 2.08849087358e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Virginia North (ftUS)
    (3686, -109.123093, 14.944862, 1e-4, 0.0, 0.0, 6.84053229634e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Virginia South
    (3687, -111.898764, 21.848820, 1e-4, 0.0, 0.0, 1.84925738722e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Virginia South (ftUS)
    (3688, -111.898764, 21.848820, 1e-4, 0.0, 0.0, 6.19314523647e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Washington North
    (3689, -127.390662, 46.808297, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Washington North (ftUS)
    (3690, -127.390662, 46.808297, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Washington South
    (3691, -126.863697, 45.151783, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Washington South (ftUS)
    (3692, -126.863697, 45.151783, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / West Virginia North
    (3693, -86.363854, 38.293447, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / West Virginia South
    (3694, -87.727921, 36.803713, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wisconsin Central
    (3695, -97.438214, 43.585869, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wisconsin Central (ftUS)
    (3696, -97.438214, 43.585869, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wisconsin North
    (3697, -97.607760, 44.907938, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wisconsin North (ftUS)
    (3698, -97.607760, 44.907938, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wisconsin South
    (3699, -97.222171, 41.765988, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wisconsin South (ftUS)
    (3700, -97.222171, 41.765988, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wisconsin Transverse Mercator
    (3701, -96.117596, 40.308550, 1e-4, 0.0, 0.0, 0.000626261142315),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wyoming East
    (3702, -107.525253, 40.475927, 1e-4, 0.0, 0.0, 3.54957583129e-06),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wyoming East Central
    (3703, -111.983494, 39.506205, 1e-4, 0.0, 0.0, 0.000162927870406),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wyoming West Central
    (3704, -115.803136, 40.284354, 1e-4, 0.0, 0.0, 0.00182561155378),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wyoming West
    (3705, -119.340930, 39.229368, 1e-4, 0.0, 0.0, 0.0180613152843),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 59N
    (3706, 166.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 60N
    (3707, 172.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 1N
    (3708, 178.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167734193383),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 2N
    (3709, -175.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 3N
    (3710, -169.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 4N
    (3711, -163.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 5N
    (3712, -157.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167734193383),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 6N
    (3713, -151.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 7N
    (3714, -145.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 8N
    (3715, -139.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 9N
    (3716, -133.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 10N
    (3717, -127.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 11N
    (3718, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 12N
    (3719, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 13N
    (3720, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 14N
    (3721, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 15N
    (3722, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 16N
    (3723, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 17N
    (3724, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 18N
    (3725, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / UTM zone 19N
    (3726, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Reunion 1947 - Reunion 1947 / TM Reunion
    (3727, 53.982673, -21.573483, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Ohio North (ftUS)
    (3728, -89.475829, 39.450489, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Ohio South (ftUS)
    (3729, -89.316674, 37.795918, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wyoming East (ftUS)
    (3730, -107.525253, 40.475927, 1e-4, 0.0, 0.0, 1.164250832e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wyoming East Central (ftUS)
    (3731, -111.983494, 39.506205, 1e-4, 0.0, 0.0, 0.00053454183786),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wyoming West Central (ftUS)
    (3732, -115.803136, 40.284354, 1e-4, 0.0, 0.0, 0.00598952723936),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Wyoming West (ftUS)
    (3733, -119.340930, 39.229368, 1e-4, 0.0, 0.0, 0.0592561650137),
    # NAD83 - NAD83 / Ohio North (ftUS)
    (3734, -89.475829, 39.450489, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Ohio South (ftUS)
    (3735, -89.316674, 37.795918, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Wyoming East (ftUS)
    (3736, -107.525253, 40.475927, 1e-4, 0.0, 0.0, 1.164250832e-05),
    # NAD83 - NAD83 / Wyoming East Central (ftUS)
    (3737, -111.983494, 39.506205, 1e-4, 0.0, 0.0, 0.00053454183786),
    # NAD83 - NAD83 / Wyoming West Central (ftUS)
    (3738, -115.803136, 40.284354, 1e-4, 0.0, 0.0, 0.00598952723936),
    # NAD83 - NAD83 / Wyoming West (ftUS)
    (3739, -119.340930, 39.229368, 1e-4, 0.0, 0.0, 0.0592561650137),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 10N
    (3740, -127.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 11N
    (3741, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 12N
    (3742, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 13N
    (3743, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 14N
    (3744, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 15N
    (3745, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 16N
    (3746, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 17N
    (3747, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 18N
    (3748, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 19N
    (3749, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 4N
    (3750, -163.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(HARN) - NAD83(HARN) / UTM zone 5N
    (3751, -157.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167734193383),
    # WGS 84 - WGS 84 / Mercator 41 (deprecated)
    (3752, 100.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Ohio North (ftUS)
    (3753, -89.475829, 39.450489, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Ohio South (ftUS)
    (3754, -89.316674, 37.795918, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Wyoming East (ftUS)
    (3755, -107.525253, 40.475927, 1e-4, 0.0, 0.0, 1.164250832e-05),
    # NAD83(HARN) - NAD83(HARN) / Wyoming East Central (ftUS)
    (3756, -111.983494, 39.506205, 1e-4, 0.0, 0.0, 0.00053454183786),
    # NAD83(HARN) - NAD83(HARN) / Wyoming West Central (ftUS)
    (3757, -115.803136, 40.284354, 1e-4, 0.0, 0.0, 0.00598952723936),
    # NAD83(HARN) - NAD83(HARN) / Wyoming West (ftUS)
    (3758, -119.340930, 39.229368, 1e-4, 0.0, 0.0, 0.0592561650137),
    # NAD83 - NAD83 / Hawaii zone 3 (ftUS)
    (3759, -162.808055, 21.098347, 1e-4, 0.0, 0.0, 0.000218613236233),
    # NAD83(HARN) - NAD83(HARN) / Hawaii zone 3 (ftUS)
    (3760, -162.808055, 21.098347, 1e-4, 0.0, 0.0, 0.000218613236233),
    # NAD83(CSRS) - NAD83(CSRS) / UTM zone 22N
    (3761, -55.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # WGS 84 - WGS 84 / South Georgia Lambert
    (3762, -37.0, -55.0, 1e-4, 0.0, 0.0, 7.25464283813e-07),
    # ETRS89 - ETRS89 / Portugal TM06
    (3763, -8.133108, 39.668258, 1e-4, 0.0, 0.0, 1e-07),
    # NZGD2000 - NZGD2000 / Chatham Island Circuit 2000
    (3764, 177.792962, -51.055907, 1e-4, 0.0, 0.0, 0.000764980679378),
    # HTRS96 - HTRS96 / Croatia TM
    (3765, 12.012600, 0.0, 1e-4, 0.0, 0.0, 0.000167486490682),
    # HTRS96 - HTRS96 / Croatia LCC
    (3766, 16.500000, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # HTRS96 - HTRS96 / UTM zone 33N
    (3767, 10.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # HTRS96 - HTRS96 / UTM zone 34N
    (3768, 16.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # Bermuda 1957 - Bermuda 1957 / UTM zone 20N
    (3769, -67.488568, 0.002677, 1e-4, 0.0, 0.0, 0.000170722079929),
    # BDA2000 - BDA2000 / Bermuda 2000 National Grid
    (3770, -70.502508, 30.969699, 1e-4, 0.0, 0.0, 0.000466227931611),
    # NAD27 - NAD27 / Alberta 3TM ref merid 111 W
    (3771, -111.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Alberta 3TM ref merid 114 W
    (3772, -114.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Alberta 3TM ref merid 117 W
    (3773, -117.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Alberta 3TM ref merid 120 W (deprecated)
    (3774, -120.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Alberta 3TM ref merid 111 W
    (3775, -111.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Alberta 3TM ref merid 114 W
    (3776, -114.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Alberta 3TM ref merid 117 W
    (3777, -117.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Alberta 3TM ref merid 120 W (deprecated)
    (3778, -120.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Alberta 3TM ref merid 111 W
    (3779, -111.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Alberta 3TM ref merid 114 W
    (3780, -114.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Alberta 3TM ref merid 117 W
    (3781, -117.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Alberta 3TM ref merid 120 W (deprecated)
    (3782, -120.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Pitcairn 2006 - Pitcairn 2006 / Pitcairn TM 2006
    (3783, -130.253869, -25.208409, 1e-4, 0.0, 0.0, 1e-07),
    # Pitcairn 1967 - Pitcairn 1967 / UTM zone 9S
    (3784, 41.729512, -89.997781, 1e-4, 0.0, 0.0, 3),
    # Popular Visualisation CRS / Mercator (deprecated)
    (3785, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unspecified GRS 1980 Authalic - World Equidistant Cyl. Sphere (deprecated)
    (3786, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # MGI - MGI / Slovene National Grid (deprecated)
    (3787, 8.660741, 44.967883, 1e-4, 0.0, 0.0, 0.00183368503349),
    # NZGD2000 - NZGD2000 / Auckland Islands TM 2000
    (3788, -180.0, -69395724704141470072832.0, 1e-4, 0.0, 0.0, 1.816775825e+21),
    # NZGD2000 - NZGD2000 / Campbell Island TM 2000
    (3789, -180.0, -69395724704141470072832.0, 1e-4, 0.0, 0.0, 1.816775825e+21),
    # NZGD2000 - NZGD2000 / Antipodes Islands TM 2000
    (3790, -180.0, -69395724704141470072832.0, 1e-4, 0.0, 0.0, 1.816775825e+21),
    # NZGD2000 - NZGD2000 / Raoul Island TM 2000
    (3791, -180.0, -69395724704141470072832.0, 1e-4, 0.0, 0.0, 1.816775825e+21),
    # NZGD2000 - NZGD2000 / Chatham Islands TM 2000
    (3793, -180.0, -69395724704141470072832.0, 1e-4, 0.0, 0.0, 1.816775825e+21),
    # Slovenia 1996 - Slovenia 1996 / Slovene National Grid
    (3794, 8.661967, 44.963745, 1e-4, 0.0, 0.0, 0.00122137626749),
    # NAD27 - NAD27 / Cuba Norte
    (3795, -85.766569, 19.747526, 1e-4, 0.0, 0.0, 1.65542587638e-07),
    # NAD27 - NAD27 / Cuba Sur
    (3796, -81.567897, 18.581116, 1e-4, 0.0, 0.0, 1.66764948517e-07),
    # NAD27 - NAD27 / MTQ Lambert
    (3797, -79.901457, 43.537872, 2e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / MTQ Lambert
    (3798, -79.901739, 43.537864, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / MTQ Lambert
    (3799, -79.901739, 43.537864, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Alberta 3TM ref merid 120 W
    (3800, -120.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Alberta 3TM ref merid 120 W
    (3801, -120.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Alberta 3TM ref merid 120 W
    (3802, -120.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / Belgian Lambert 2008
    (3812, -3.779987, 44.504140, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Mississippi TM
    (3814, -94.546047, 20.698279, 1e-4, 0.0, 0.0, 5.93323493376e-05),
    # NAD83(HARN) - NAD83(HARN) / Mississippi TM
    (3815, -94.546047, 20.698279, 1e-4, 0.0, 0.0, 5.93323493376e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Mississippi TM
    (3816, -94.546047, 20.698279, 1e-4, 0.0, 0.0, 5.93323493376e-05),
    # HD1909
    (3819, 0.001330, 0.005482, 1e-4, 0.0, 0.0, 0.00210485876581),
    # TWD67
    (3821, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # TWD97
    (3824, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # TWD97 - TWD97 / TM2 zone 119
    (3825, 116.754566, 0.0, 1e-4, 0.0, 0.0, 3.77451942768e-06),
    # TWD97 - TWD97 / TM2 zone 121
    (3826, 118.754566, 0.0, 1e-4, 0.0, 0.0, 3.77451942768e-06),
    # TWD67 - TWD67 / TM2 zone 119
    (3827, 116.754574, 0.0, 1e-4, 0.0, 0.0, 3.77630931325e-06),
    # TWD67 - TWD67 / TM2 zone 121
    (3828, 118.754574, 0.0, 1e-4, 0.0, 0.0, 3.77630931325e-06),
    # Hu Tzu Shan 1950 - Hu Tzu Shan 1950 / UTM zone 51N
    (3829, 118.518815, -0.001836, 1e-4, 0.0, 0.0, 0.000168838771146),
    # WGS 84 - WGS 84 / PDC Mercator
    (3832, 150.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Gauss-Kruger zone 2
    (3833, -12.900861, -0.000653, 1e-4, 0.0, 0.0, 19),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / Gauss-Kruger zone 2
    (3834, -12.900886, -0.000705, 1e-4, 0.0, 0.0, 19),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / Gauss-Kruger zone 3
    (3835, -14.962339, -0.000705, 1e-4, 0.0, 0.0, 325),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / Gauss-Kruger zone 4
    (3836, -16.408682, -0.000705, 1e-4, 0.0, 0.0, 2788),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 3
    (3837, -20.962237, -0.000640, 1e-4, 0.0, 0.0, 325),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 4
    (3838, -25.408531, -0.000634, 1e-4, 0.0, 0.0, 2788),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 9
    (3839, -32.938043, -0.000623, 1e-4, 0.0, 0.0, 1736227),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 10
    (3840, -27.808166, -0.000630, 1e-4, 0.0, 0.0, 3900661),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 6
    (3841, -32.069200, -0.000705, 1e-4, 0.0, 0.0, 66222),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 7 (deprecated)
    (3842, -32.069200, -0.000705, 1e-4, 0.0, 0.0, 66222),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 8 (deprecated)
    (3843, -32.069200, -0.000705, 1e-4, 0.0, 0.0, 66222),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Stereo70
    (3844, 19.031114, 41.338747, 1e-4, 0.0, 0.0, 0.000235576793784),
    # SWEREF99 - SWEREF99 / RT90 7.5 gon V emulation
    (3845, -2.045275, 0.005870, 1e-4, 0.0, 0.0, 0.290864143382),
    # SWEREF99 - SWEREF99 / RT90 5 gon V emulation
    (3846, 0.204568, 0.005869, 1e-4, 0.0, 0.0, 0.290894214323),
    # SWEREF99 - SWEREF99 / RT90 2.5 gon V emulation
    (3847, 2.454412, 0.005874, 1e-4, 0.0, 0.0, 0.290924331489),
    # SWEREF99 - SWEREF99 / RT90 0 gon emulation
    (3848, 4.704257, 0.005884, 1e-4, 0.0, 0.0, 0.290954007263),
    # SWEREF99 - SWEREF99 / RT90 2.5 gon O emulation
    (3849, 6.954103, 0.005901, 1e-4, 0.0, 0.0, 0.290983685827),
    # SWEREF99 - SWEREF99 / RT90 5 gon O emulation
    (3850, 9.203949, 0.005917, 1e-4, 0.0, 0.0, 0.291013318266),
    # NZGD2000 - NZGD2000 / NZCS2000
    (3851, 45.451938, -76.574216, 1e-4, 0.0, 0.0, 1e-07),
    # RSRGD2000 - RSRGD2000 / DGLC2000
    (3852, 64.997688, -85.670790, 1e-4, 0.0, 0.0, 1e-07),
    # SWEREF99 - County ST74
    (3854, 16.334074, 58.611250, 1e-4, 0.0, 0.0, 4.26211045124e-07),
    # WGS 84 - WGS 84 / Pseudo-Mercator
    (3857, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / GK19FIN
    (3873, 127.145362, 0.0, 1e-4, 0.0, 0.0, 4),
    # ETRS89 - ETRS89 / GK20FIN
    (3874, -13.227627, 0.0, 1e-4, 0.0, 0.0, 24859380.5865),
    # ETRS89 - ETRS89 / GK21FIN
    (3875, 58.173609, 0.0, 1e-4, 0.0, 0.0, 38947934.4903),
    # ETRS89 - ETRS89 / GK22FIN
    (3876, 41.716493, 0.0, 1e-4, 0.0, 0.0, 37109685.1616),
    # ETRS89 - ETRS89 / GK23FIN
    (3877, 10.691535, 0.0, 1e-4, 0.0, 0.0, 33178639.0139),
    # ETRS89 - ETRS89 / GK24FIN
    (3878, 53.256414, 0.0, 1e-4, 0.0, 0.0, 41864081.1787),
    # ETRS89 - ETRS89 / GK25FIN
    (3879, -85.436796, 0.0, 1e-4, 0.0, 0.0, 3),
    # ETRS89 - ETRS89 / GK26FIN
    (3880, 79.076868, 0.0, 1e-4, 0.0, 0.0, 50233746.8753),
    # ETRS89 - ETRS89 / GK27FIN
    (3881, -26.906819, 0.0, 1e-4, 0.0, 0.0, 30536734.4885),
    # ETRS89 - ETRS89 / GK28FIN
    (3882, 127.464196, 0.0, 1e-4, 0.0, 0.0, 4),
    # ETRS89 - ETRS89 / GK29FIN
    (3883, 20.539419, 0.0, 1e-4, 0.0, 0.0, 42832061.2365),
    # ETRS89 - ETRS89 / GK30FIN
    (3884, -118.669193, 0.0, 1e-4, 0.0, 0.0, 4),
    # ETRS89 - ETRS89 / GK31FIN
    (3885, -27.090251, 0.0, 1e-4, 0.0, 0.0, 35314528.0022),
    # IGRS
    (3889, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGRS - IGRS / UTM zone 37N
    (3890, 34.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # IGRS - IGRS / UTM zone 38N
    (3891, 40.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # IGRS - IGRS / UTM zone 39N
    (3892, 46.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ED50 - ED50 / Iraq National Grid
    (3893, 38.317477, 28.775835, 1e-4, 0.0, 0.0, 0.00224584298847),
    # MGI 1901
    (3906, -0.001824, 0.004341, 1e-4, 0.0, 0.0, 1e-07),
    # MGI 1901 - MGI 1901 / Balkans zone 5
    (3907, -29.150765, 0.004341, 1e-4, 0.0, 0.0, 15676),
    # MGI 1901 - MGI 1901 / Balkans zone 6
    (3908, -32.075110, 0.004341, 1e-4, 0.0, 0.0, 66337),
    # MGI 1901 - MGI 1901 / Balkans zone 7
    (3909, -34.002668, 0.004341, 1e-4, 0.0, 0.0, 229299),
    # MGI 1901 - MGI 1901 / Balkans zone 8
    (3910, -34.544950, 0.004341, 1e-4, 0.0, 0.0, 676616),
    # MGI 1901 - MGI 1901 / Slovenia Grid
    (3911, 10.509170, 0.004341, 1e-4, 0.0, 0.0, 0.000166780751983),
    # MGI 1901 - MGI 1901 / Slovene National Grid
    (3912, 8.656858, 44.967799, 1e-4, 0.0, 0.0, 0.00122265677783),
    # Puerto Rico - Puerto Rico / UTM zone 20N
    (3920, -67.488356, -0.000913, 1e-4, 0.0, 0.0, 0.000170718587512),
    # RGF93 - RGF93 / CC42
    (3942, -14.311604, 29.720396, 1e-4, 0.0, 0.0, 1e-07),
    # RGF93 - RGF93 / CC43
    (3943, -12.599616, 22.170442, 1e-4, 0.0, 0.0, 1.72178260982e-07),
    # RGF93 - RGF93 / CC44
    (3944, -11.145663, 14.991504, 1e-4, 0.0, 0.0, 1.962762326e-07),
    # RGF93 - RGF93 / CC45
    (3945, -9.899261, 8.280630, 1e-4, 0.0, 0.0, 1e-07),
    # RGF93 - RGF93 / CC46
    (3946, -8.821866, 2.090957, 1e-4, 0.0, 0.0, 6.83264806867e-06),
    # RGF93 - RGF93 / CC47
    (3947, -7.883615, -3.560344, 1e-4, 0.0, 0.0, 1e-07),
    # RGF93 - RGF93 / CC48
    (3948, -7.061060, -8.682510, 1e-4, 0.0, 0.0, 1.96625478566e-07),
    # RGF93 - RGF93 / CC49
    (3949, -6.335567, -13.302168, 1e-4, 0.0, 0.0, 2.73459590971e-07),
    # RGF93 - RGF93 / CC50
    (3950, -5.692163, -17.455955, 1e-4, 0.0, 0.0, 3.12575139105e-07),
    # NAD83 - NAD83 / Virginia Lambert
    (3968, -79.500000, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Virginia Lambert
    (3969, -79.500000, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Virginia Lambert
    (3970, -79.500000, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / NSIDC EASE-Grid North (deprecated)
    (3973, 0.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / NSIDC EASE-Grid South (deprecated)
    (3974, 0.0, -90.0, 1e-4, 0.0, 0.0, 0.28512494266),
    # WGS 84 - WGS 84 / NSIDC EASE-Grid Global (deprecated)
    (3975, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / NSIDC Sea Ice Polar Stereographic South
    (3976, 0.0, -90.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Canada Atlas Lambert
    (3978, -95.0, 49.0, 1e-4, 0.0, 0.0, 2.22843933739e-06),
    # NAD83(CSRS) - NAD83(CSRS) / Canada Atlas Lambert
    (3979, -95.0, 49.0, 1e-4, 0.0, 0.0, 2.22843933739e-06),
    # Katanga 1955 - Katanga 1955 / Katanga Lambert (deprecated)
    (3985, 21.614914, 4.667143, 1e-4, 0.0, 0.0, 1e-07),
    # Katanga 1955 - Katanga 1955 / Katanga Gauss zone A
    (3986, 28.153235, -13.515030, 1e-4, 0.0, 0.0, 7.21804099157e-07),
    # Katanga 1955 - Katanga 1955 / Katanga Gauss zone B
    (3987, 26.153204, -13.515033, 1e-4, 0.0, 0.0, 7.23244738765e-07),
    # Katanga 1955 - Katanga 1955 / Katanga Gauss zone C
    (3988, 24.153173, -13.515036, 1e-4, 0.0, 0.0, 7.21585820429e-07),
    # Katanga 1955 - Katanga 1955 / Katanga Gauss zone D
    (3989, 22.153141, -13.515038, 1e-4, 0.0, 0.0, 7.21323885955e-07),
    # Puerto Rico - Puerto Rico State Plane CS of 1927
    (3991, -67.870589, 17.825983, 1e-4, 0.0, 0.0, 5.52230405401e-07),
    # Puerto Rico - Puerto Rico / St. Croix
    (3992, -67.868336, 17.550603, 1e-4, 0.0, 0.0, 5.52302085453e-07),
    # WGS 84 - WGS 84 / Mercator 41
    (3994, 100.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / Arctic Polar Stereographic
    (3995, 0.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / IBCAO Polar Stereographic
    (3996, 0.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / Dubai Local TM
    (3997, 50.846381, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # Unknown datum based upon the Airy 1830 ellipsoid
    (4001, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Airy Modified 1849 ellipsoid
    (4002, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Australian National Spheroid
    (4003, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Bessel 1841 ellipsoid
    (4004, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Bessel Modified ellipsoid
    (4005, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Bessel Namibia ellipsoid
    (4006, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Clarke 1858 ellipsoid
    (4007, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Clarke 1866 ellipsoid
    (4008, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Clarke 1866 Michigan ellipsoid
    (4009, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Clarke 1880 (Benoit) ellipsoid
    (4010, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Clarke 1880 (IGN) ellipsoid
    (4011, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Clarke 1880 (RGS) ellipsoid
    (4012, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Clarke 1880 (Arc) ellipsoid
    (4013, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Clarke 1880 (SGA 1922) ellipsoid
    (4014, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Everest 1830 (1937 Adjustment) ellipsoid
    (4015, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Everest 1830 (1967 Definition) ellipsoid
    (4016, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Everest 1830 Modified ellipsoid
    (4018, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the GRS 1980 ellipsoid
    (4019, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Helmert 1906 ellipsoid
    (4020, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Indonesian National Spheroid
    (4021, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the International 1924 ellipsoid
    (4022, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # MOLDREF99
    (4023, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Krassowsky 1940 ellipsoid
    (4024, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the NWL 9D ellipsoid
    (4025, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # MOLDREF99 - MOLDREF99 / Moldova TM
    (4026, 25.858420, 45.109884, 1e-4, 0.0, 0.0, 5.48255047761e-06),
    # Unknown datum based upon the Plessis 1817 ellipsoid
    (4027, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Struve 1860 ellipsoid
    (4028, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the War Office ellipsoid
    (4029, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the WGS 84 ellipsoid
    (4030, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the GEM 10C ellipsoid
    (4031, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the OSU86F ellipsoid
    (4032, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the OSU91A ellipsoid
    (4033, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Clarke 1880 ellipsoid
    (4034, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Authalic Sphere
    (4035, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the GRS 1967 ellipsoid
    (4036, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / TMzn35N
    (4037, 22.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / TMzn36N
    (4038, 28.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # Unknown datum based upon the Average Terrestrial System 1977 ellipsoid
    (4041, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Everest (1830 Definition) ellipsoid
    (4042, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the WGS 72 ellipsoid
    (4043, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Everest 1830 (1962 Definition) ellipsoid
    (4044, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unknown datum based upon the Everest 1830 (1975 Definition) ellipsoid
    (4045, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGRDC 2005
    (4046, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unspecified datum based upon the GRS 1980 Authalic Sphere
    (4047, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGRDC 2005 - RGRDC 2005 / Congo TM zone 12
    (4048, -180.0, -1745366112639232000.0, 1e-4, 0.0, 0.0, 4.56935779774e+16),
    # RGRDC 2005 - RGRDC 2005 / Congo TM zone 14
    (4049, -180.0, -1745366112639232000.0, 1e-4, 0.0, 0.0, 4.56935779774e+16),
    # RGRDC 2005 - RGRDC 2005 / Congo TM zone 16
    (4050, -180.0, -1745366112639232000.0, 1e-4, 0.0, 0.0, 4.56935779774e+16),
    # RGRDC 2005 - RGRDC 2005 / Congo TM zone 18
    (4051, -180.0, -1745366112639232000.0, 1e-4, 0.0, 0.0, 4.56935779774e+16),
    # Unspecified datum based upon the Clarke 1866 Authalic Sphere
    (4052, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unspecified datum based upon the International 1924 Authalic Sphere
    (4053, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unspecified datum based upon the Hughes 1980 ellipsoid
    (4054, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Popular Visualisation CRS
    (4055, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGRDC 2005 - RGRDC 2005 / Congo TM zone 20
    (4056, -180.0, -1745366112639232000.0, 1e-4, 0.0, 0.0, 4.56935779774e+16),
    # RGRDC 2005 - RGRDC 2005 / Congo TM zone 22
    (4057, -180.0, -1745366112639232000.0, 1e-4, 0.0, 0.0, 4.56935779774e+16),
    # RGRDC 2005 - RGRDC 2005 / Congo TM zone 24
    (4058, -180.0, -1745366112639232000.0, 1e-4, 0.0, 0.0, 4.56935779774e+16),
    # RGRDC 2005 - RGRDC 2005 / Congo TM zone 26
    (4059, -180.0, -1745366112639232000.0, 1e-4, 0.0, 0.0, 4.56935779774e+16),
    # RGRDC 2005 - RGRDC 2005 / Congo TM zone 28
    (4060, -180.0, -1745366112639232000.0, 1e-4, 0.0, 0.0, 4.56935779774e+16),
    # RGRDC 2005 - RGRDC 2005 / UTM zone 33S
    (4061, 15.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGRDC 2005 - RGRDC 2005 / UTM zone 34S
    (4062, 21.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGRDC 2005 - RGRDC 2005 / UTM zone 35S
    (4063, 27.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Chua - Chua / UTM zone 23S
    (4071, 120.589775, -89.997469, 3e-1, 0.0, 0.0, 752569),
    # SREF98
    (4075, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # REGCAN95
    (4081, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # REGCAN95 - REGCAN95 / UTM zone 27N
    (4082, -25.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # REGCAN95 - REGCAN95 / UTM zone 28N
    (4083, -19.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # WGS 84 - WGS 84 / World Equidistant Cylindrical
    (4087, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Unspecified GRS 1980 Authalic Sphere - World Equidistant Cyl. (Sphere)
    (4088, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / DKTM1
    (4093, 6.458601, 45.108089, 1e-4, 0.0, 0.0, 5.48054231331e-06),
    # ETRS89 - ETRS89 / DKTM2
    (4094, 4.924698, 45.023454, 1e-4, 0.0, 0.0, 0.000250038283411),
    # ETRS89 - ETRS89 / DKTM3
    (4095, 4.155650, 44.883129, 1e-4, 0.0, 0.0, 0.00518374890089),
    # ETRS89 - ETRS89 / DKTM4
    (4096, 4.908895, 44.687324, 1e-4, 0.0, 0.0, 0.0584785356768),
    # Greek
    (4120, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # GGRS87
    (4121, 0.000672, 0.002230, 1e-4, 0.0, 0.0, 1e-07),
    # ATS77
    (4122, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # KKJ
    (4123, -0.001123, -0.001198, 1e-4, 0.0, 0.0, 0.00045256830617),
    # RT90
    (4124, -0.001580, 0.004856, 1e-4, 0.0, 0.0, 0.0121206376536),
    # Samboja
    (4125, 0.006161, 0.000411, 1e-4, 0.0, 0.0, 1e-07),
    # LKS94 (ETRS89)
    (4126, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Tete
    (4127, -0.000898, -0.002062, 3e-4, 0.0, 0.0, 1e-07),
    # Madzansua
    (4128, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Observatario
    (4129, -0.000988, -0.003030, 4e-3, 0.0, 0.0, 1e-07),
    # Moznet
    (4130, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Indian 1960
    (4131, 0.007915, 0.002867, 1e-4, 0.0, 0.0, 1e-07),
    # FD58
    (4132, -0.001470, 0.003582, 1e-4, 0.0, 0.0, 1e-07),
    # EST92
    (4133, -0.000007, -0.000002, 1e-4, 0.0, 0.0, 1e-07),
    # PSD93
    (4134, 0.000290, 0.002104, 1e-4, 0.0, 0.0, 0.0164364721444),
    # Old Hawaiian
    (4135, -0.002560, -0.001637, 1e-4, 0.0, 0.0, 1e-07),
    # St. Lawrence Island
    (4136, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # St. Paul Island
    (4137, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # St. George Island
    (4138, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Puerto Rico
    (4139, 0.000647, -0.000913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS98)
    (4140, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Israel 1993
    (4141, 0.000494, 0.000470, 1e-4, 0.0, 0.0, 1e-07),
    # Locodjo 1965
    (4142, 0.000476, 0.004223, 1e-4, 0.0, 0.0, 1e-07),
    # Abidjan 1987
    (4143, 0.000476, 0.004222, 1e-4, 0.0, 0.0, 1e-07),
    # Kalianpur 1937
    (4144, 0.007223, 0.002424, 1e-4, 0.0, 0.0, 1e-07),
    # Kalianpur 1962
    (4145, 0.006127, 0.002089, 1e-4, 0.0, 0.0, 1e-07),
    # Kalianpur 1975
    (4146, 0.006612, 0.002324, 1e-4, 0.0, 0.0, 1e-07),
    # Hanoi 1972
    (4147, -0.000973, -0.000564, 1e-4, 0.0, 0.0, 1e-07),
    # Hartebeesthoek94
    (4148, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # CH1903
    (4149, 0.000136, 0.003665, 1e-4, 0.0, 0.0, 1e-07),
    # CH1903+
    (4150, 0.000135, 0.003666, 1e-4, 0.0, 0.0, 1e-07),
    # CHTRF95
    (4151, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN)
    (4152, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Rassadiran
    (4153, -0.001415, -0.001434, 1e-4, 0.0, 0.0, 1e-07),
    # ED50(ED77)
    (4154, -0.001186, -0.001483, 1e-4, 0.0, 0.0, 1e-07),
    # Dabola 1981
    (4155, 0.000332, 0.001121, 1e-4, 0.0, 0.0, 1e-07),
    # S-JTSK
    (4156, 0.000683, 0.004341, 1e-4, 0.0, 0.0, 1e-07),
    # Mount Dillon
    (4157, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Naparima 1955
    (4158, 0.003342, 0.001553, 1e-4, 0.0, 0.0, 1e-07),
    # ELD79
    (4159, -0.000890, -0.001379, 1e-4, 0.0, 0.0, 1e-07),
    # Chos Malal 1914
    (4160, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Pampa del Castillo
    (4161, 0.000126, 0.001686, 1e-4, 0.0, 0.0, 1e-07),
    # Korean 1985
    (4162, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Yemen NGN96
    (4163, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # South Yemen
    (4164, -0.001240, 0.000606, 1e-4, 0.0, 0.0, 1e-07),
    # Bissau
    (4165, 0.002273, 0.000244, 1e-4, 0.0, 0.0, 1e-07),
    # Korean 1995
    (4166, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NZGD2000
    (4167, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Accra
    (4168, 0.000287, 0.002912, 1e-4, 0.0, 0.0, 1e-07),
    # American Samoa 1962
    (4169, 0.001060, 0.003853, 1e-4, 0.0, 0.0, 1e-07),
    # SIRGAS 1995
    (4170, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGF93
    (4171, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # POSGAR
    (4172, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IRENET95
    (4173, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Sierra Leone 1924
    (4174, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Sierra Leone 1968
    (4175, 0.000036, 0.000913, 1e-4, 0.0, 0.0, 1e-07),
    # Australian Antarctic
    (4176, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Pulkovo 1942(83)
    (4178, -0.001087, -0.000705, 1e-4, 0.0, 0.0, 1e-07),
    # Pulkovo 1942(58)
    (4179, -0.001082, -0.000675, 1e-4, 0.0, 0.0, 0.000160818642846),
    # EST97
    (4180, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Luxembourg 1930
    (4181, 0.000870, 0.000478, 1e-4, 0.0, 0.0, 0.00360010809676),
    # Azores Occidental 1939
    (4182, -0.001518, 0.000733, 1e-4, 0.0, 0.0, 1e-07),
    # Azores Central 1948
    (4183, 0.001500, -0.000344, 1e-4, 0.0, 0.0, 1e-07),
    # Azores Oriental 1940
    (4184, 0.001267, 0.000479, 1e-4, 0.0, 0.0, 1e-07),
    # Madeira 1936
    (4185, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # OSNI 1952
    (4188, -0.001348, 0.005166, 1e-4, 0.0, 0.0, 9.9823269413e-05),
    # REGVEN
    (4189, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # POSGAR 98
    (4190, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Albanian 1987
    (4191, 0.000972, 0.000409, 2e-3, 0.0, 0.0, 0.00443379148624),
    # Douala 1948
    (4192, -0.001569, -0.000793, 1e-4, 0.0, 0.0, 1e-07),
    # Manoca 1962
    (4193, -0.001364, -0.000374, 1e-4, 0.0, 0.0, 1e-07),
    # Qornoq 1927
    (4194, 0.001240, -0.001709, 1e-4, 0.0, 0.0, 1e-07),
    # Scoresbysund 1952
    (4195, 0.003154, -0.000927, 1e-4, 0.0, 0.0, 0.000149003695697),
    # Ammassalik 1958
    (4196, 0.003972, -0.000032, 1e-4, 0.0, 0.0, 0.00014900509268),
    # Garoua
    (4197, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Kousseri
    (4198, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Egypt 1930
    (4199, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Pulkovo 1995
    (4200, -0.001140, -0.000738, 1e-4, 0.0, 0.0, 3.80119308829e-06),
    # Adindan
    (4201, -0.000135, 0.001845, 1e-4, 0.0, 0.0, 1e-07),
    # AGD66
    (4202, -0.000398, 0.001121, 1e-4, 0.0, 0.0, 5.70448561956e-05),
    # AGD84
    (4203, -0.000431, 0.001348, 1e-4, 0.0, 0.0, 1e-07),
    # Ain el Abd
    (4204, -0.002120, 0.000063, 1e-4, 0.0, 0.0, 1e-07),
    # Afgooye
    (4205, -0.001464, 0.000407, 1e-4, 0.0, 0.0, 1e-07),
    # Agadez
    (4206, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Lisbon
    (4207, -0.000544, 0.000937, 1e-4, 0.0, 0.0, 1e-07),
    # Aratu
    (4208, 0.002578, -0.001333, 1e-4, 0.0, 0.0, 1e-07),
    # Arc 1950
    (4209, -0.000808, -0.002659, 1e-4, 0.0, 0.0, 1e-07),
    # Arc 1960
    (4210, -0.000054, -0.002731, 1e-4, 0.0, 0.0, 1e-07),
    # Batavia
    (4211, 0.006119, -0.000452, 1e-4, 0.0, 0.0, 1e-07),
    # Barbados 1938
    (4212, 0.002704, 0.003791, 1e-4, 0.0, 0.0, 1e-07),
    # Beduaram
    (4213, -0.000782, 0.001700, 1e-4, 0.0, 0.0, 1e-07),
    # Beijing 1954
    (4214, -0.001387, -0.000744, 1e-4, 0.0, 0.0, 1e-07),
    # Belge 1950
    (4215, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Bermuda 1957
    (4216, 0.001913, 0.002677, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / BLM 59N (ftUS)
    (4217, 166.511256, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # Bogota 1975
    (4218, 0.002731, -0.002876, 1e-4, 0.0, 0.0, 1e-07),
    # Bukit Rimpah
    (4219, 0.005966, -0.000434, 1e-4, 0.0, 0.0, 1e-07),
    # Camacupa
    (4220, -0.003123, -0.002089, 1e-4, 0.0, 0.0, 1e-07),
    # Campo Inchauspe
    (4221, 0.001222, 0.000814, 1e-4, 0.0, 0.0, 1e-07),
    # Cape
    (4222, -0.000970, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Carthage
    (4223, 0.000054, 0.003898, 1e-4, 0.0, 0.0, 1e-07),
    # Chua
    (4224, 0.002186, -0.000303, 2e-4, 0.0, 0.0, 1e-07),
    # Corrego Alegre 1970-72
    (4225, 0.001516, -0.000037, 1e-4, 0.0, 0.0, 1e-07),
    # Cote d'Ivoire
    (4226, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Deir ez Zor
    (4227, 0.000077, 0.002159, 1e-4, 0.0, 0.0, 1e-07),
    # Douala
    (4228, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Egypt 1907
    (4229, 0.000988, -0.000118, 1e-4, 0.0, 0.0, 1e-07),
    # ED50
    (4230, -0.000880, -0.001094, 1e-4, 0.0, 0.0, 1e-07),
    # ED87
    (4231, -0.000862, -0.001048, 1e-4, 0.0, 0.0, 8.92673714537e-07),
    # Fahud
    (4232, 0.000055, 0.002086, 1e-4, 0.0, 0.0, 6.90151937306e-05),
    # Gandajika 1970
    (4233, -0.002884, 0.000452, 1e-4, 0.0, 0.0, 1e-07),
    # Garoua
    (4234, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Guyane Francaise
    (4235, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hu Tzu Shan 1950
    (4236, -0.004932, -0.001836, 1e-4, 0.0, 0.0, 1e-07),
    # HD72
    (4237, -0.000645, -0.000135, 1e-4, 0.0, 0.0, 1e-07),
    # ID74
    (4238, -0.000135, 0.000045, 1e-4, 0.0, 0.0, 1e-07),
    # Indian 1954
    (4239, 0.007394, 0.002704, 1e-4, 0.0, 0.0, 1e-07),
    # Indian 1975
    (4240, 0.007313, 0.002614, 1e-4, 0.0, 0.0, 1e-07),
    # Jamaica 1875
    (4241, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # JAD69
    (4242, 0.001859, 0.003522, 1e-4, 0.0, 0.0, 1e-07),
    # Kalianpur 1880
    (4243, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Kandawala
    (4244, 0.007071, 0.000778, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968
    (4245, 0.007646, 0.000045, 1e-4, 0.0, 0.0, 1e-07),
    # KOC
    (4246, -0.001798, 0.004753, 1e-4, 0.0, 0.0, 1e-07),
    # La Canoa
    (4247, 0.000994, -0.003237, 1e-4, 0.0, 0.0, 1e-07),
    # PSAD56
    (4248, 0.001572, -0.003400, 1e-4, 0.0, 0.0, 1e-07),
    # Lake
    (4249, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Leigon
    (4250, 0.000261, 0.003292, 1e-4, 0.0, 0.0, 1e-07),
    # Liberia 1964
    (4251, 0.000359, 0.000796, 1e-4, 0.0, 0.0, 1e-07),
    # Lome
    (4252, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Luzon 1911
    (4253, -0.000692, -0.000461, 1e-4, 0.0, 0.0, 1e-07),
    # Hito XVIII 1963
    (4254, 0.001761, 0.000841, 1e-4, 0.0, 0.0, 1e-07),
    # Herat North
    (4255, -0.001994, 0.001031, 1e-4, 0.0, 0.0, 1e-07),
    # Mahe 1971
    (4256, -0.001976, -0.001212, 1e-4, 0.0, 0.0, 1e-07),
    # Makassar
    (4257, 0.004670, 0.001318, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89
    (4258, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Malongo 1987
    (4259, -0.000048, -0.000907, 1e-4, 0.0, 0.0, 1e-07),
    # Manoca
    (4260, -0.001364, -0.000374, 1e-4, 0.0, 0.0, 1e-07),
    # Merchich
    (4261, 0.001312, 0.000425, 1e-4, 0.0, 0.0, 1e-07),
    # Massawa
    (4262, 0.003638, 0.000543, 1e-4, 0.0, 0.0, 1e-07),
    # Minna
    (4263, -0.000835, 0.001103, 1e-4, 0.0, 0.0, 1e-07),
    # Mhast
    (4264, -0.000037, -0.000872, 1e-4, 0.0, 0.0, 1e-07),
    # Monte Mario
    (4265, -0.000243, 0.000726, 1e-4, 0.0, 0.0, 0.00202813293551),
    # M'poraloko
    (4266, -0.001168, 0.000380, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27
    (4267, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27 Michigan
    (4268, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83
    (4269, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Nahrwan 1967
    (4270, -0.001401, 0.003446, 1e-4, 0.0, 0.0, 1e-07),
    # Naparima 1972
    (4271, 0.003369, 0.001492, 1e-4, 0.0, 0.0, 1e-07),
    # NZGD49
    (4272, 0.000239, 0.001723, 1e-4, 0.0, 0.0, 0.000238056902695),
    # NGO 1948
    (4273, -0.001001, 0.004277, 1e-4, 0.0, 0.0, 0.00982479538944),
    # Datum 73
    (4274, 0.000990, 0.000331, 1e-4, 0.0, 0.0, 1e-07),
    # NTF
    (4275, -0.000539, 0.002894, 1e-4, 0.0, 0.0, 1e-07),
    # NSWC 9Z-2
    (4276, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # OSGB 1936
    (4277, -0.000890, 0.004833, 1e-4, 0.0, 0.0, 0.000173131335665),
    # OSGB70
    (4278, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # OS(SN)80
    (4279, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Padang
    (4280, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Palestine 1923
    (4281, -0.002432, 0.004319, 1e-4, 0.0, 0.0, 0.035817049683),
    # Pointe Noire
    (4282, 0.000458, -0.002632, 1e-4, 0.0, 0.0, 1e-07),
    # GDA94
    (4283, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Pulkovo 1942
    (4284, -0.001041, -0.000829, 1e-4, 0.0, 0.0, 0.00017875386402),
    # Qatar 1974
    (4285, -0.002537, 0.000198, 1e-4, 0.0, 0.0, 1e-07),
    # Qatar 1948
    (4286, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Qornoq
    (4287, 0.001240, -0.001709, 1e-4, 0.0, 0.0, 1e-07),
    # Loma Quintana
    (4288, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Amersfoort
    (4289, -0.000069, 0.004114, 1e-4, 0.0, 0.0, 0.000819106524051),
    # SAD69
    (4291, 0.000009, -0.000371, 1e-4, 0.0, 0.0, 1e-07),
    # Sapper Hill 1943
    (4292, 0.000189, 0.000651, 1e-4, 0.0, 0.0, 1e-07),
    # Schwarzeck
    (4293, 0.000871, -0.002270, 1e-4, 0.0, 0.0, 1e-07),
    # Segora
    (4294, 0.006146, 0.000371, 1e-4, 0.0, 0.0, 1e-07),
    # Serindung
    (4295, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Sudan
    (4296, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Tananarive
    (4297, -0.002174, -0.000823, 1e-4, 0.0, 0.0, 1e-07),
    # Timbalai 1948
    (4298, 0.006011, -0.000434, 1e-4, 0.0, 0.0, 1e-07),
    # TM65
    (4299, -0.001349, 0.005166, 1e-4, 0.0, 0.0, 9.98218724291e-05),
    # TM75
    (4300, -0.001349, 0.005166, 1e-4, 0.0, 0.0, 9.98218724291e-05),
    # Tokyo
    (4301, 0.004558, 0.006155, 1e-4, 0.0, 0.0, 1e-07),
    # Trinidad 1903
    (4302, 0.002556, 0.004269, 1e-4, 0.0, 0.0, 1e-07),
    # TC(1948)
    (4303, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Voirol 1875
    (4304, -0.002219, 0.002053, 1e-4, 0.0, 0.0, 1e-07),
    # Bern 1938
    (4306, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Nord Sahara 1959
    (4307, -0.000628, 0.002687, 1e-4, 0.0, 0.0, 0.00279660945808),
    # RT38
    (4308, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Yacare
    (4309, 0.001536, 0.000335, 1e-4, 0.0, 0.0, 1e-07),
    # Yoff
    (4310, 0.001707, 0.000805, 2e-3, 0.0, 0.0, 1e-07),
    # Zanderij
    (4311, 0.001078, -0.003238, 1e-4, 0.0, 0.0, 1e-07),
    # MGI
    (4312, 0.002281, 0.003783, 1e-4, 0.0, 0.0, 0.00679735265744),
    # Belge 1972
    (4313, 0.000982, -0.000810, 1e-4, 0.0, 0.0, 0.00081014464398),
    # DHDN
    (4314, -0.000020, 0.003770, 1e-4, 0.0, 0.0, 0.00135560762378),
    # Conakry 1905
    (4315, 0.002327, -0.000081, 1e-4, 0.0, 0.0, 1e-07),
    # Dealul Piscului 1930
    (4316, -0.000902, -0.002778, 1e-4, 0.0, 0.0, 1e-07),
    # Dealul Piscului 1970
    (4317, -0.001087, -0.000696, 1e-4, 0.0, 0.0, 1e-07),
    # NGN
    (4318, -0.000051, 0.000025, 1e-4, 0.0, 0.0, 1e-07),
    # KUDAMS
    (4319, 0.000102, 0.000022, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 72
    (4322, 0.000154, 0.000041, 1e-4, 0.0, 0.0, 6.90179876983e-05),
    # WGS 72BE
    (4324, 0.000226, 0.000017, 1e-4, 0.0, 0.0, 0.000148999504745),
    # WGS 84
    (4326, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968 - Kertau 1968 / Johor Grid
    # (4390, 103.561061, 2.042479, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968 - Kertau 1968 / Sembilan and Melaka Grid
    # (4391, 101.941856, 2.720706, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968 - Kertau 1968 / Pahang Grid
    # (4392, 102.434625, 3.710743, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968 - Kertau 1968 / Selangor Grid
    # (4393, 101.702581, 3.174023, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968 - Kertau 1968 / Terengganu Grid
    # (4394, 102.893593, 4.945822, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968 - Kertau 1968 / Pinang Grid
    # (4395, 100.344588, 5.420964, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968 - Kertau 1968 / Kedah and Perlis Grid
    # (4396, 100.636273, 5.964747, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968 - Kertau 1968 / Perak Revised Grid
    # (4397, 100.815426, 3.652217, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968 - Kertau 1968 / Kelantan Grid
    # (4398, 102.175768, 5.893531, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / BLM 59N (ftUS)
    (4399, 166.511305, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 60N (ftUS)
    (4400, 172.511305, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 1N (ftUS)
    (4401, 178.511305, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 2N (ftUS)
    (4402, -175.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 3N (ftUS)
    (4403, -169.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 4N (ftUS)
    (4404, -163.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560116419656),
    # NAD27 - NAD27 / BLM 5N (ftUS)
    (4405, -157.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 6N (ftUS)
    (4406, -151.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 7N (ftUS)
    (4407, -145.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 8N (ftUS)
    (4408, -139.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560116419656),
    # NAD27 - NAD27 / BLM 9N (ftUS)
    (4409, -133.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 10N (ftUS)
    (4410, -127.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 11N (ftUS)
    (4411, -121.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 12N (ftUS)
    (4412, -115.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560095508481),
    # NAD27 - NAD27 / BLM 13N (ftUS)
    (4413, -109.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560095508481),
    # NAD83(HARN) - NAD83(HARN) / Guam Map Grid
    (4414, 143.832817, 11.690660, 1e-4, 0.0, 0.0, 1e-07),
    # Katanga 1955 - Katanga 1955 / Katanga Lambert
    (4415, 21.391778, -13.493076, 1e-4, 0.0, 0.0, 1.63185177371e-07),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 7
    (4417, -33.997621, -0.000705, 1e-4, 0.0, 0.0, 228895),
    # NAD27 - NAD27 / BLM 18N (ftUS)
    (4418, -79.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560095508481),
    # NAD27 - NAD27 / BLM 19N (ftUS)
    (4419, -73.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560095508481),
    # NAD83 - NAD83 / BLM 60N (ftUS)
    (4420, 172.511256, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 1N (ftUS)
    (4421, 178.511256, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 2N (ftUS)
    (4422, -175.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 3N (ftUS)
    (4423, -169.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 4N (ftUS)
    (4424, -163.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 5N (ftUS)
    (4425, -157.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 6N (ftUS)
    (4426, -151.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 7N (ftUS)
    (4427, -145.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 8N (ftUS)
    (4428, -139.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550303349519),
    # NAD83 - NAD83 / BLM 9N (ftUS)
    (4429, -133.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 10N (ftUS)
    (4430, -127.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 11N (ftUS)
    (4431, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 12N (ftUS)
    (4432, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550282438344),
    # NAD83 - NAD83 / BLM 13N (ftUS)
    (4433, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 8
    (4434, -34.542258, -0.000705, 1e-4, 0.0, 0.0, 675426),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Puerto Rico and Virgin Is.
    (4437, -68.300715, 16.017453, 1e-4, 0.0, 0.0, 1.52795109898e-07),
    # NAD83 - NAD83 / BLM 18N (ftUS)
    (4438, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550296474612),
    # NAD83 - NAD83 / BLM 19N (ftUS)
    (4439, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD27 - NAD27 / Pennsylvania South
    (4455, -84.804155, 39.113951, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / New York Long Island
    (4456, -81.145849, 40.001471, 2e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / South Dakota North (ftUS)
    (4457, -107.437658, 43.585145, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / Australian Centre for Remote Sensing Lambert
    (4462, 132.0, -27.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGSPM06
    (4463, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGSPM06 - RGSPM06 / UTM zone 21N
    (4467, -61.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # RGM04
    (4470, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGM04 - RGM04 / UTM zone 38S
    (4471, 45.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Combani 1950 - Cadastre 1997 / UTM zone 38S (deprecated)
    (4474, -171.220028, -89.996540, 1e-4, 0.0, 0.0, 752569),
    # Cadastre 1997
    (4475, -0.000517, -0.002321, 1e-4, 0.0, 0.0, 1e-07),
    # Mexico ITRF92
    (4483, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Mexico ITRF92 - Mexico ITRF92 / UTM zone 11N
    (4484, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # Mexico ITRF92 - Mexico ITRF92 / UTM zone 12N
    (4485, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Mexico ITRF92 - Mexico ITRF92 / UTM zone 13N
    (4486, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Mexico ITRF92 - Mexico ITRF92 / UTM zone 14N
    (4487, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Mexico ITRF92 - Mexico ITRF92 / UTM zone 15N
    (4488, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Mexico ITRF92 - Mexico ITRF92 / UTM zone 16N
    (4489, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # China Geodetic Coordinate System 2000
    (4490, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 13
    (4491, 74.628964, 0.0, 1e-4, 0.0, 0.0, 20188044.2593),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 14
    (4492, 135.198090, 0.0, 1e-4, 0.0, 0.0, 32544772.6287),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 15
    (4493, -131.122818, 0.0, 1e-4, 0.0, 0.0, 4),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 16
    (4494, 10.090780, 0.0, 1e-4, 0.0, 0.0, 1926710),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 17
    (4495, -142.087915, 0.0, 1e-4, 0.0, 0.0, 4),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 18
    (4496, 157.079150, 0.0, 1e-4, 0.0, 0.0, 37963013.9291),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 19
    (4497, -140.854638, 0.0, 1e-4, 0.0, 0.0, 4),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 20
    (4498, 83.772373, 0.0, 1e-4, 0.0, 0.0, 24859380.5865),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 21
    (4499, 160.173609, 0.0, 1e-4, 0.0, 0.0, 38947934.4903),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 22
    (4500, 148.716493, 0.0, 1e-4, 0.0, 0.0, 37109685.1616),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger zone 23
    (4501, 122.691535, 0.0, 1e-4, 0.0, 0.0, 33178639.0139),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 75E
    (4502, 70.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 81E
    (4503, 76.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 87E
    (4504, 82.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 93E
    (4505, 88.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 99E
    (4506, 94.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 105E
    (4507, 100.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 111E
    (4508, 106.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 117E
    (4509, 112.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 123E
    (4510, 118.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 129E
    (4511, 124.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coordinate System 2000 - CGCS2000 / Gauss-Kruger CM 135E
    (4512, 130.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 25
    (4513, -35.436796, 0.0, 1e-4, 0.0, 0.0, 0.927733159518),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 26
    (4514, 131.076868, 0.0, 1e-4, 0.0, 0.0, 50233746.8753),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 27
    (4515, 27.093181, 0.0, 1e-4, 0.0, 0.0, 30536734.4885),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 28
    (4516, -176.535804, 0.0, 1e-4, 0.0, 0.0, 5),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 29
    (4517, 78.539419, 0.0, 1e-4, 0.0, 0.0, 42832061.2365),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 30
    (4518, -58.669193, 0.0, 1e-4, 0.0, 0.0, 2),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 31
    (4519, 34.909749, 0.0, 1e-4, 0.0, 0.0, 35314528.0022),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 32
    (4520, -59.955843, 0.0, 1e-4, 0.0, 0.0, 2),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 33
    (4521, -0.916188, 0.0, 1e-4, 0.0, 0.0, 0.0239857363916),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 34
    (4522, -119.897896, 0.0, 1e-4, 0.0, 0.0, 4),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 35
    (4523, 21.302299, 0.0, 1e-4, 0.0, 0.0, 29924692.2451),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 36
    (4524, -164.303513, 0.0, 1e-4, 0.0, 0.0, 80840240.8144),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 37
    (4525, -123.934667, 0.0, 1e-4, 0.0, 0.0, 4),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 38
    (4526, 40.206782, 0.0, 1e-4, 0.0, 0.0, 39878539.7262),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 39
    (4527, -63.518144, 0.0, 1e-4, 0.0, 0.0, 2),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 40
    (4528, -30.334321, 0.0, 1e-4, 0.0, 0.0, 0.794150663215),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 41
    (4529, -92.888130, 0.0, 1e-4, 0.0, 0.0, 3),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 42
    (4530, -34.761554, 0.0, 1e-4, 0.0, 0.0, 0.910055352226),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 43
    (4531, 96.343375, 0.0, 1e-4, 0.0, 0.0, 59473095.3069),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 44
    (4532, -4.242089, 0.0, 1e-4, 0.0, 0.0, 0.111057619358),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger zone 45
    (4533, -170.654135, 0.0, 1e-4, 0.0, 0.0, 79086319.0025),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 75E
    (4534, 70.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 78E
    (4535, 73.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 81E
    (4536, 76.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 84E
    (4537, 79.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 87E
    (4538, 82.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 90E
    (4539, 85.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 93E
    (4540, 88.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 96E
    (4541, 91.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 99E
    (4542, 94.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 102E
    (4543, 97.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 105E
    (4544, 100.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 108E
    (4545, 103.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 111E
    (4546, 106.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 114E
    (4547, 109.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 117E
    (4548, 112.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 120E
    (4549, 115.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 123E
    (4550, 118.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 126E
    (4551, 121.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 129E
    (4552, 124.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 132E
    (4553, 127.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # China Geodetic Coor System 2000 - CGCS2000 / 3-degree Gauss-Kruger CM 135E
    (4554, 130.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167403981322),
    # New Beijing
    (4555, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RRAF 1991
    (4558, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RRAF 1991 - RRAF 1991 / UTM zone 20N
    (4559, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # New Beijing - New Beijing / Gauss-Kruger zone 13
    (4568, 74.619430, 0.0, 1e-4, 0.0, 0.0, 20186451.1937),
    # New Beijing - New Beijing / Gauss-Kruger zone 14
    (4569, 135.181195, 0.0, 1e-4, 0.0, 0.0, 32540211.6008),
    # New Beijing - New Beijing / Gauss-Kruger zone 15
    (4570, -131.151239, 0.0, 1e-4, 0.0, 0.0, 4),
    # New Beijing - New Beijing / Gauss-Kruger zone 16
    (4571, 10.044873, 0.0, 1e-4, 0.0, 0.0, 1897470),
    # New Beijing - New Beijing / Gauss-Kruger zone 17
    (4572, -142.159629, 0.0, 1e-4, 0.0, 0.0, 4),
    # New Beijing - New Beijing / Gauss-Kruger zone 18
    (4573, 156.970261, 0.0, 1e-4, 0.0, 0.0, 37933969.7856),
    # New Beijing - New Beijing / Gauss-Kruger zone 19
    (4574, -141.015939, 0.0, 1e-4, 0.0, 0.0, 4),
    # New Beijing - New Beijing / Gauss-Kruger zone 20
    (4575, 83.538576, 0.0, 1e-4, 0.0, 0.0, 24812506.305),
    # New Beijing - New Beijing / Gauss-Kruger zone 21
    (4576, 159.841233, 0.0, 1e-4, 0.0, 0.0, 38878499.3142),
    # New Beijing - New Beijing / Gauss-Kruger zone 22
    (4577, 148.252119, 0.0, 1e-4, 0.0, 0.0, 37027455.3704),
    # New Beijing - New Beijing / Gauss-Kruger zone 23
    (4578, 122.052854, 0.0, 1e-4, 0.0, 0.0, 33069293.0703),
    # New Beijing - New Beijing / Gauss-Kruger CM 75E
    (4579, 70.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167351769051),
    # New Beijing - New Beijing / Gauss-Kruger CM 81E
    (4580, 76.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / Gauss-Kruger CM 87E
    (4581, 82.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167351769051),
    # New Beijing - New Beijing / Gauss-Kruger CM 93E
    (4582, 88.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / Gauss-Kruger CM 99E
    (4583, 94.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / Gauss-Kruger CM 105E
    (4584, 100.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / Gauss-Kruger CM 111E
    (4585, 106.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / Gauss-Kruger CM 117E
    (4586, 112.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167351769051),
    # New Beijing - New Beijing / Gauss-Kruger CM 123E
    (4587, 118.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / Gauss-Kruger CM 129E
    (4588, 124.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / Gauss-Kruger CM 135E
    (4589, 130.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167349673575),
    # Anguilla 1957
    (4600, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Antigua 1943
    (4601, -0.000135, 0.000642, 1e-4, 0.0, 0.0, 1e-07),
    # Dominica 1945
    (4602, 0.006153, 0.004847, 1e-4, 0.0, 0.0, 1e-07),
    # Grenada 1953
    (4603, 0.001920, 0.000841, 1e-4, 0.0, 0.0, 1e-07),
    # Montserrat 1958
    (4604, 0.003225, 0.003301, 1e-4, 0.0, 0.0, 1e-07),
    # St. Kitts 1955
    (4605, 0.001644, 0.002134, 1e-4, 0.0, 0.0, 1e-07),
    # St. Lucia 1955
    (4606, 0.001150, 0.002677, 1e-4, 0.0, 0.0, 1e-07),
    # St. Vincent 1945
    (4607, 0.002987, 0.002483, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27(76)
    (4608, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD27(CGQ77)
    (4609, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Xian 1980
    (4610, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hong Kong 1980
    (4611, -0.002810, -0.000835, 1e-4, 0.0, 0.0, 0.00143402954673),
    # JGD2000
    (4612, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Segara
    (4613, 0.006146, 0.000371, 1e-4, 0.0, 0.0, 1e-07),
    # QND95
    (4614, -0.002423, -0.000148, 1e-4, 0.0, 0.0, 0.000277101740043),
    # Porto Santo
    (4615, -0.002237, 0.002840, 1e-4, 0.0, 0.0, 1e-07),
    # Selvagem Grande
    (4616, -0.001114, 0.000543, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS)
    (4617, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # SAD69
    (4618, 0.000039, -0.000348, 1e-4, 0.0, 0.0, 1e-07),
    # SWEREF99
    (4619, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Point 58
    (4620, -0.001159, 0.001492, 1e-4, 0.0, 0.0, 1e-07),
    # Fort Marigot
    (4621, 0.002228, -0.003889, 1e-4, 0.0, 0.0, 1e-07),
    # Guadeloupe 1948
    (4622, -0.000144, -0.002713, 1e-4, 0.0, 0.0, 1e-07),
    # CSG67
    (4623, 0.002066, 0.000995, 1e-4, 0.0, 0.0, 1e-07),
    # RGFG95
    (4624, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Martinique 1938
    (4625, 0.004330, 0.001366, 1e-4, 0.0, 0.0, 1e-07),
    # Reunion 1947
    (4626, -0.008516, -0.011413, 1e-4, 0.0, 0.0, 1e-07),
    # RGR92
    (4627, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Tahiti 52
    (4628, 0.001051, 0.001393, 1e-4, 0.0, 0.0, 1e-07),
    # Tahaa 54
    (4629, 0.003262, 0.000472, 1e-4, 0.0, 0.0, 0.00024470595681),
    # IGN72 Nuku Hiva
    (4630, 0.002461, 0.000588, 1e-4, 0.0, 0.0, 1e-07),
    # K0 1949
    (4631, -0.001680, 0.000931, 1e-4, 0.0, 0.0, 1e-07),
    # Combani 1950
    (4632, -0.000530, -0.002369, 1e-4, 0.0, 0.0, 1e-07),
    # IGN56 Lifou
    (4633, 0.001999, -0.002088, 1e-4, 0.0, 0.0, 1e-07),
    # IGN72 Grand Terre
    (4634, -0.003126, 0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # ST87 Ouvea
    (4635, -0.003280, 0.002324, 1e-4, 0.0, 0.0, 0.0128661031417),
    # Petrels 1972
    (4636, 0.001743, 0.001501, 1e-4, 0.0, 0.0, 1e-07),
    # Perroud 1950
    (4637, 0.001383, 0.001555, 1e-4, 0.0, 0.0, 1e-07),
    # Saint Pierre et Miquelon 1950
    (4638, 0.003863, 0.003328, 1e-4, 0.0, 0.0, 1e-07),
    # MOP78
    (4639, -0.001186, -0.001148, 1e-4, 0.0, 0.0, 1e-07),
    # RRAF 1991
    (4640, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGN53 Mare
    (4641, 0.001597, -0.001225, 1e-4, 0.0, 0.0, 1e-07),
    # ST84 Ile des Pins
    (4642, -0.003126, 0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # ST71 Belep
    (4643, -0.005059, -0.011461, 1e-4, 0.0, 0.0, 0.0951689131837),
    # NEA74 Noumea
    (4644, -0.003148, 0.002635, 1e-4, 0.0, 0.0, 1e-07),
    # RGNC 1991
    (4645, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Grand Comoros
    (4646, 0.004582, -0.003247, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / UTM zone 32N (zE-N)
    (4647, 11.361590, 0.0, 1e-4, 0.0, 0.0, 49144291.1282),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 25
    (4652, -36.595730, 0.0, 1e-4, 0.0, 0.0, 0.958073961567),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 26
    (4653, 129.544265, 0.0, 1e-4, 0.0, 0.0, 49820287.8768),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 27
    (4654, 25.088580, 0.0, 1e-4, 0.0, 0.0, 29965077.9258),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 28
    (4655, -179.131291, 0.0, 1e-4, 0.0, 0.0, 5),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 29
    (4656, 75.210312, 0.0, 1e-4, 0.0, 0.0, 42267195.8383),
    # Reykjavik 1900
    (4657, 0.001788, 0.000045, 1e-4, 0.0, 0.0, 1e-07),
    # Hjorsey 1955
    (4658, 0.000422, -0.000751, 1e-4, 0.0, 0.0, 1e-07),
    # ISN93
    (4659, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Helle 1954
    (4660, -0.000548, 0.003948, 1e-4, 0.0, 0.0, 0.313312377064),
    # LKS92
    (4661, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGN72 Grande Terre
    (4662, -0.003131, 0.002640, 1e-4, 0.0, 0.0, 1e-07),
    # Porto Santo 1995
    (4663, -0.002223, 0.002828, 1e-4, 0.0, 0.0, 1e-07),
    # Azores Oriental 1995
    (4664, 0.001259, 0.000499, 1e-4, 0.0, 0.0, 1e-07),
    # Azores Central 1995
    (4665, 0.001494, -0.000343, 1e-4, 0.0, 0.0, 1e-07),
    # Lisbon 1890
    (4666, -0.001716, 0.005112, 1e-4, 0.0, 0.0, 1e-07),
    # IKBD-92
    (4667, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # ED79
    (4668, -0.000880, -0.001076, 1e-4, 0.0, 0.0, 1e-07),
    # LKS94
    (4669, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGM95
    (4670, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Voirol 1879
    (4671, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Chatham Islands 1971
    (4672, -0.000341, 0.001022, 1e-4, 0.0, 0.0, 1e-07),
    # Chatham Islands 1979
    (4673, -0.000075, 0.001018, 1e-4, 0.0, 0.0, 6.90193846822e-05),
    # SIRGAS 2000
    (4674, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Guam 1963
    (4675, -0.002228, 0.002342, 1e-4, 0.0, 0.0, 1e-07),
    # Vientiane 1982
    (4676, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Lao 1993
    (4677, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Lao 1997
    (4678, -0.001179, -0.000358, 1e-4, 0.0, 0.0, 1e-07),
    # Jouik 1961
    (4679, 0.002275, 0.002633, 1e-4, 0.0, 0.0, 1e-07),
    # Nouakchott 1965
    (4680, -0.000570, -0.002541, 1e-4, 0.0, 0.0, 1e-07),
    # Mauritania 1999
    (4681, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Gulshan 303
    (4682, 0.006611, 0.002362, 1e-4, 0.0, 0.0, 1e-07),
    # PRS92
    (4683, -0.000166, -0.001797, 1e-4, 0.0, 0.0, 0.00596583606649),
    # Gan 1970
    (4684, -0.002884, 0.000452, 1e-4, 0.0, 0.0, 1e-07),
    # Gandajika
    (4685, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # MAGNA-SIRGAS
    (4686, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGPF
    (4687, -0.000006, -0.000002, 1e-4, 0.0, 0.0, 1e-07),
    # Fatu Iva 72
    (4688, 0.007073, 0.003959, 1e-4, 0.0, 0.0, 2),
    # IGN63 Hiva Oa
    (4689, 0.000680, 0.000073, 1e-4, 0.0, 0.0, 0.00134327020431),
    # Tahiti 79
    (4690, 0.001130, 0.001987, 1e-4, 0.0, 0.0, 0.000607215570847),
    # Moorea 87
    (4691, 0.001022, 0.002067, 1e-4, 0.0, 0.0, 0.000944907978407),
    # Maupiti 83
    (4692, 0.000781, 0.000217, 1e-4, 0.0, 0.0, 1e-07),
    # Nakhl-e Ghanem
    (4693, -0.000001, 0.000006, 1e-4, 0.0, 0.0, 1e-07),
    # POSGAR 94
    (4694, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Katanga 1955
    (4695, -0.000086, -0.002315, 1e-4, 0.0, 0.0, 1e-07),
    # Kasai 1953
    (4696, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGC 1962 6th Parallel South
    (4697, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGN 1962 Kerguelen
    (4698, -0.001680, 0.000931, 1e-4, 0.0, 0.0, 1e-07),
    # Le Pouce 1934
    (4699, 0.001423, -0.004506, 1e-4, 0.0, 0.0, 1e-07),
    # IGN Astro 1960
    (4700, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGCB 1955
    (4701, -0.001419, -0.001527, 1e-4, 0.0, 0.0, 1e-07),
    # Mauritania 1999
    (4702, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Mhast 1951
    (4703, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Mhast (onshore)
    (4704, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Mhast (offshore)
    (4705, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Egypt Gulf of Suez S-650 TL
    (4706, 0.001012, 0.000037, 1e-4, 0.0, 0.0, 1e-07),
    # Tern Island 1961
    (4707, -0.001042, -0.003011, 1e-4, 0.0, 0.0, 1e-07),
    # Cocos Islands 1965
    (4708, -0.000198, 0.003934, 1e-4, 0.0, 0.0, 1e-07),
    # Iwo Jima 1945
    (4709, 0.000674, -0.002460, 1e-4, 0.0, 0.0, 1e-07),
    # St. Helena 1971
    (4710, 0.004941, -0.004468, 1e-4, 0.0, 0.0, 1e-07),
    # Marcus Island 1952
    (4711, -0.002102, -0.000226, 1e-4, 0.0, 0.0, 1e-07),
    # Ascension Island 1958
    (4712, 0.000961, 0.000479, 1e-4, 0.0, 0.0, 1e-07),
    # Ayabelle Lighthouse
    (4713, -0.001150, 0.001284, 1e-4, 0.0, 0.0, 1e-07),
    # Bellevue
    (4714, -0.006908, 0.004269, 1e-4, 0.0, 0.0, 1e-07),
    # Camp Area Astro
    (4715, -0.001159, 0.002161, 1e-4, 0.0, 0.0, 1e-07),
    # Phoenix Islands 1966
    (4716, -0.002731, -0.003391, 1e-4, 0.0, 0.0, 1e-07),
    # Cape Canaveral
    (4717, 0.001356, 0.001637, 1e-4, 0.0, 0.0, 1e-07),
    # Solomon 1968
    (4718, -0.001788, -0.006800, 1e-4, 0.0, 0.0, 1e-07),
    # Easter Island 1967
    (4719, 0.001320, 0.001004, 1e-4, 0.0, 0.0, 1e-07),
    # Fiji 1986
    (4720, 0.000154, 0.000041, 1e-4, 0.0, 0.0, 6.90179876983e-05),
    # Fiji 1956
    (4721, 0.003458, -0.001755, 1e-4, 0.0, 0.0, 1e-07),
    # South Georgia 1968
    (4722, 0.001069, -0.002695, 1e-4, 0.0, 0.0, 1e-07),
    # GCGD59
    (4723, 0.001055, -0.002532, 1e-4, 0.0, 0.0, 0.0231938212529),
    # Diego Garcia 1969
    (4724, -0.003907, -0.002071, 1e-4, 0.0, 0.0, 1e-07),
    # Johnston Island 1961
    (4725, -0.000710, -0.001827, 1e-4, 0.0, 0.0, 1e-07),
    # SIGD61
    (4726, 0.000349, 0.002280, 1e-4, 0.0, 0.0, 0.00318375837109),
    # Midway 1961
    (4727, -0.000728, 0.002505, 1e-4, 0.0, 0.0, 1e-07),
    # Pico de las Nieves 1984
    (4728, -0.000826, 0.001149, 1e-4, 0.0, 0.0, 1e-07),
    # Pitcairn 1967
    (4729, 0.001482, 0.000380, 1e-4, 0.0, 0.0, 1e-07),
    # Santo 1965
    (4730, 0.000377, 0.000760, 1e-4, 0.0, 0.0, 1e-07),
    # Viti Levu 1916
    (4731, 0.003512, -0.000326, 1e-4, 0.0, 0.0, 1e-07),
    # Marshall Islands 1960
    (4732, 0.000467, -0.000344, 1e-4, 0.0, 0.0, 1e-07),
    # Wake Island 1952
    (4733, -0.000512, 0.001347, 1e-4, 0.0, 0.0, 1e-07),
    # Tristan 1968
    (4734, 0.003935, -0.005508, 1e-4, 0.0, 0.0, 1e-07),
    # Kusaie 1951
    (4735, 0.015961, -0.010164, 1e-4, 0.0, 0.0, 1e-07),
    # Deception Island
    (4736, 0.000108, -0.001329, 1e-4, 0.0, 0.0, 1e-07),
    # Korea 2000
    (4737, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hong Kong 1963
    (4738, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Hong Kong 1963(67)
    (4739, -0.002434, -0.001709, 1e-4, 0.0, 0.0, 1e-07),
    # PZ-90
    (4740, 0.000021, 0.000014, 1e-4, 0.0, 0.0, 1.29779800773e-06),
    # FD54
    (4741, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # GDM2000
    (4742, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Karbala 1979
    (4743, -0.003017, 0.002378, 1e-4, 0.0, 0.0, 1e-07),
    # Nahrwan 1934
    (4744, -0.001302, 0.003349, 4e-3, 0.0, 0.0, 1e-07),
    # RD/83
    (4745, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # PD/83
    (4746, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # GR96
    (4747, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Vanua Levu 1915
    (4748, 0.003512, -0.000326, 1e-4, 0.0, 0.0, 1e-07),
    # RGNC91-93
    (4749, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # ST87 Ouvea
    (4750, 0.000145, -0.000207, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau (RSO)
    (4751, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Viti Levu 1912
    (4752, 0.003503, -0.000199, 2e-4, 0.0, 0.0, 1e-07),
    # fk89
    (4753, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # LGD2006
    (4754, -0.000987, -0.000023, 1e-4, 0.0, 0.0, 1e-07),
    # DGN95
    (4755, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # VN-2000
    (4756, -0.000352, -0.001002, 1e-4, 0.0, 0.0, 1e-07),
    # SVY21
    (4757, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # JAD2001
    (4758, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007)
    (4759, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 66
    (4760, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # HTRS96
    (4761, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # BDA2000
    (4762, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Pitcairn 2006
    (4763, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RSRGD2000
    (4764, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Slovenia 1996
    (4765, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 30
    (4766, -62.902159, 0.0, 1e-4, 0.0, 0.0, 2),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 31
    (4767, 29.571119, 0.0, 1e-4, 0.0, 0.0, 33569247.8905),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 32
    (4768, -66.638007, 0.0, 1e-4, 0.0, 0.0, 2),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 33
    (4769, -9.220773, 0.0, 1e-4, 0.0, 0.0, 0.24139927099),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 34
    (4770, -130.150259, 0.0, 1e-4, 0.0, 0.0, 4),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 35
    (4771, 8.724364, 0.0, 1e-4, 0.0, 0.0, 0.228403314515),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 36
    (4772, -179.643782, 0.0, 1e-4, 0.0, 0.0, 71962992.2841),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 37
    (4773, -142.540113, 0.0, 1e-4, 0.0, 0.0, 4),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 38
    (4774, 17.759496, 0.0, 1e-4, 0.0, 0.0, 0.464942529704),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 39
    (4775, -90.466149, 0.0, 1e-4, 0.0, 0.0, 3),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 40
    (4776, -62.533235, 0.0, 1e-4, 0.0, 0.0, 2),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 41
    (4777, -131.189274, 0.0, 1e-4, 0.0, 0.0, 4),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 42
    (4778, -80.127970, 0.0, 1e-4, 0.0, 0.0, 3),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 43
    (4779, 42.825519, 0.0, 1e-4, 0.0, 0.0, 40256829.9606),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 44
    (4780, -67.132926, 0.0, 1e-4, 0.0, 0.0, 2),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger zone 45
    (4781, 115.711990, 0.0, 1e-4, 0.0, 0.0, 64966193.2191),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 75E
    (4782, 70.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167351769051),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 78E
    (4783, 73.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 81E
    (4784, 76.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 84E
    (4785, 79.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 87E
    (4786, 82.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167351769051),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 90E
    (4787, 85.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 93E
    (4788, 88.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 96E
    (4789, 91.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167351769051),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 99E
    (4790, 94.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 102E
    (4791, 97.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 105E
    (4792, 100.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 108E
    (4793, 103.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167351769051),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 111E
    (4794, 106.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 114E
    (4795, 109.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 117E
    (4796, 112.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167351769051),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 120E
    (4797, 115.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 123E
    (4798, 118.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 126E
    (4799, 121.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 129E
    (4800, 124.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # Bern 1898 (Bern)
    (4801, 7.438933, 0.003665, 1e-4, 0.0, 0.0, 1e-07),
    # Bogota 1975 (Bogota)
    (4802, -74.077516, -0.002876, 1e-4, 0.0, 0.0, 1e-07),
    # Lisbon (Lisbon)
    (4803, -9.132877, 0.000937, 1e-4, 0.0, 0.0, 1e-07),
    # Makassar (Jakarta)
    (4804, 106.811424, 0.001318, 1e-4, 0.0, 0.0, 1e-07),
    # MGI (Ferro)
    (4805, -17.666545, 0.004341, 1e-4, 0.0, 0.0, 1e-07),
    # Monte Mario (Rome)
    (4806, 12.452303, 0.000766, 1e-4, 0.0, 0.0, 0.00221724772314),
    # NTF (Paris)
    (4807, 2.336752, 0.002894, 1e-4, 0.0, 0.0, 1e-07),
    # Padang (Jakarta)
    (4808, 106.807719, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Belge 1950 (Brussels)
    (4809, 4.367975, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Tananarive (Paris)
    (4810, 2.335126, -0.000823, 1e-4, 0.0, 0.0, 1e-07),
    # Voirol 1875 (Paris)
    (4811, 2.335039, 0.002053, 1e-4, 0.0, 0.0, 1e-07),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 132E
    (4812, 127.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167353864526),
    # Batavia (Jakarta)
    (4813, 106.809192, -0.000452, 1e-4, 0.0, 0.0, 1e-07),
    # RT38 (Stockholm)
    (4814, 18.058278, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Greek (Athens)
    (4815, 23.716338, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Carthage (Paris)
    (4816, 2.337379, 0.003898, 1e-4, 0.0, 0.0, 1e-07),
    # NGO 1948 (Oslo)
    (4817, 10.721436, 0.004688, 1e-4, 0.0, 0.0, 0.0102768050441),
    # S-JTSK (Ferro)
    (4818, -17.664410, 0.004341, 1e-4, 0.0, 0.0, 1e-07),
    # Nord Sahara 1959 (Paris)
    (4819, 2.336679, 0.002687, 1e-4, 0.0, 0.0, 0.0027917897635),
    # Segara (Jakarta)
    (4820, 106.809408, 0.000371, 1e-4, 0.0, 0.0, 1e-07),
    # Voirol 1879 (Paris)
    (4821, 2.337229, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # New Beijing - New Beijing / 3-degree Gauss-Kruger CM 135E
    (4822, 130.513124, 0.0, 1e-4, 0.0, 0.0, 0.000167349673575),
    # Sao Tome
    (4823, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Principe
    (4824, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / Cape Verde National
    (4826, -25.500000, 14.666667, 1e-4, 0.0, 0.0, 1.53340806719e-07),
    # ETRS89 - ETRS89 / LCC Germany (N-E)
    (4839, 10.500000, 51.0, 1e-4, 0.0, 0.0, 1.57945152274e-06),
    # ETRS89 - ETRS89 / NTM zone 5 (deprecated)
    (4855, 4.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 6 (deprecated)
    (4856, 5.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 7 (deprecated)
    (4857, 6.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 8 (deprecated)
    (4858, 7.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 9 (deprecated)
    (4859, 8.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 10 (deprecated)
    (4860, 9.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 11 (deprecated)
    (4861, 10.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 12 (deprecated)
    (4862, 11.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 13 (deprecated)
    (4863, 12.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 14 (deprecated)
    (4864, 13.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 15 (deprecated)
    (4865, 14.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 16 (deprecated)
    (4866, 15.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 17 (deprecated)
    (4867, 16.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 18 (deprecated)
    (4868, 17.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 19 (deprecated)
    (4869, 18.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 20 (deprecated)
    (4870, 19.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 21 (deprecated)
    (4871, 20.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 22 (deprecated)
    (4872, 21.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 23 (deprecated)
    (4873, 22.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 24 (deprecated)
    (4874, 23.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 25 (deprecated)
    (4875, 24.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 26 (deprecated)
    (4876, 25.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 27 (deprecated)
    (4877, 26.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 28 (deprecated)
    (4878, 27.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 29 (deprecated)
    (4879, 28.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 30 (deprecated)
    (4880, 29.590493, -9.041817, 1e-4, 0.0, 0.0, 1e-07),
    # ATF (Paris)
    (4901, 2.337208, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NDG (Paris)
    (4902, 2.337229, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Madrid 1870 (Madrid)
    (4903, -3.687939, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Lisbon 1890 (Lisbon)
    (4904, -9.132876, 0.005112, 1e-4, 0.0, 0.0, 1e-07),
    # PTRA08
    (5013, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # PTRA08 - PTRA08 / UTM zone 25N
    (5014, -37.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # PTRA08 - PTRA08 / UTM zone 26N
    (5015, -31.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # PTRA08 - PTRA08 / UTM zone 28N
    (5016, -19.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # Lisbon - Lisbon / Portuguese Grid New
    (5018, -8.133106, 39.668258, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / UPS North (E,N)
    (5041, -45.0, 64.916412, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / UPS South (E,N)
    (5042, -135.0, -64.916412, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / TM35FIN(N,E)
    (5048, 22.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # NAD27 - NAD27 / Conus Albers
    (5069, -96.0, 23.0, 4e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Conus Albers
    (5070, -96.0, 23.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Conus Albers
    (5071, -96.0, 23.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Conus Albers
    (5072, -96.0, 23.0, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / NTM zone 5
    (5105, 4.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50765117723e-07),
    # ETRS89 - ETRS89 / NTM zone 6
    (5106, 5.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50765117723e-07),
    # ETRS89 - ETRS89 / NTM zone 7
    (5107, 6.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50765117723e-07),
    # ETRS89 - ETRS89 / NTM zone 8
    (5108, 7.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50830601342e-07),
    # ETRS89 - ETRS89 / NTM zone 9
    (5109, 8.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50765117723e-07),
    # ETRS89 - ETRS89 / NTM zone 10
    (5110, 9.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50765117723e-07),
    # ETRS89 - ETRS89 / NTM zone 11
    (5111, 10.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 12
    (5112, 11.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50765117723e-07),
    # ETRS89 - ETRS89 / NTM zone 13
    (5113, 12.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50765117723e-07),
    # ETRS89 - ETRS89 / NTM zone 14
    (5114, 13.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50765117723e-07),
    # ETRS89 - ETRS89 / NTM zone 15
    (5115, 14.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 16
    (5116, 15.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 17
    (5117, 16.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 18
    (5118, 17.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 19
    (5119, 18.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 20
    (5120, 19.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 21
    (5121, 20.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 22
    (5122, 21.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 23
    (5123, 22.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 24
    (5124, 23.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 25
    (5125, 24.133150, 49.006786, 1e-4, 0.0, 0.0, 1.51267158799e-07),
    # ETRS89 - ETRS89 / NTM zone 26
    (5126, 25.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 27
    (5127, 26.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 28
    (5128, 27.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 29
    (5129, 28.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # ETRS89 - ETRS89 / NTM zone 30
    (5130, 29.133150, 49.006786, 1e-4, 0.0, 0.0, 1.50939740706e-07),
    # Tokyo 1892
    (5132, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Korean 1985 - Korean 1985 / East Sea Belt
    (5167, 128.848244, 33.474487, 1e-4, 0.0, 0.0, 1.83629163075e-06),
    # Korean 1985 - Korean 1985 / Central Belt Jeju
    (5168, 124.859263, 33.023936, 1e-4, 0.0, 0.0, 1.7503334675e-06),
    # Tokyo 1892 - Tokyo 1892 / Korea West Belt
    (5169, 122.848244, 33.474487, 1e-4, 0.0, 0.0, 1.83284282684e-06),
    # Tokyo 1892 - Tokyo 1892 / Korea Central Belt
    (5170, 124.848244, 33.474487, 1e-4, 0.0, 0.0, 1.83284282684e-06),
    # Tokyo 1892 - Tokyo 1892 / Korea East Belt
    (5171, 126.848244, 33.474487, 1e-4, 0.0, 0.0, 1.83284282684e-06),
    # Tokyo 1892 - Tokyo 1892 / Korea East Sea Belt
    (5172, 128.848244, 33.474487, 1e-4, 0.0, 0.0, 1.83629163075e-06),
    # Korean 1985 - Korean 1985 / Modified West Belt
    (5173, 122.851135, 33.474487, 1e-4, 0.0, 0.0, 1.83284282684e-06),
    # Korean 1985 - Korean 1985 / Modified Central Belt
    (5174, 124.851135, 33.474487, 1e-4, 0.0, 0.0, 1.83284282684e-06),
    # Korean 1985 - Korean 1985 / Modified Central Belt Jeju
    (5175, 124.862153, 33.023936, 1e-4, 0.0, 0.0, 1.7503334675e-06),
    # Korean 1985 - Korean 1985 / Modified East Belt
    (5176, 126.851135, 33.474487, 1e-4, 0.0, 0.0, 1.83284282684e-06),
    # Korean 1985 - Korean 1985 / Modified East Sea Belt
    (5177, 128.851135, 33.474487, 1e-4, 0.0, 0.0, 1.83284282684e-06),
    # Korean 1985 - Korean 1985 / Unified CS
    (5178, 117.991609, 19.692586, 1e-4, 0.0, 0.0, 0.00541401165538),
    # Korea 2000 - Korea 2000 / Unified CS
    (5179, 117.992603, 19.694477, 1e-4, 0.0, 0.0, 0.0054025677382),
    # Korea 2000 - Korea 2000 / West Belt
    (5180, 122.848488, 33.474969, 1e-4, 0.0, 0.0, 1.83891097549e-06),
    # Korea 2000 - Korea 2000 / Central Belt
    (5181, 124.848488, 33.474969, 1e-4, 0.0, 0.0, 1.83891097549e-06),
    # Korea 2000 - Korea 2000 / Central Belt Jeju
    (5182, 124.859504, 33.024466, 1e-4, 0.0, 0.0, 1.75640161615e-06),
    # Korea 2000 - Korea 2000 / East Belt
    (5183, 126.848488, 33.474969, 1e-4, 0.0, 0.0, 1.83891097549e-06),
    # Korea 2000 - Korea 2000 / East Sea Belt
    (5184, 128.848488, 33.474969, 1e-4, 0.0, 0.0, 1.84240343515e-06),
    # Korea 2000 - Korea 2000 / West Belt 2010
    (5185, 122.870278, 32.573927, 1e-4, 0.0, 0.0, 1.67865073308e-06),
    # Korea 2000 - Korea 2000 / Central Belt 2010
    (5186, 124.870278, 32.573927, 1e-4, 0.0, 0.0, 1.67865073308e-06),
    # Korea 2000 - Korea 2000 / East Belt 2010
    (5187, 126.870278, 32.573927, 1e-4, 0.0, 0.0, 1.67865073308e-06),
    # Korea 2000 - Korea 2000 / East Sea Belt 2010
    (5188, 128.870278, 32.573927, 1e-4, 0.0, 0.0, 1.67865073308e-06),
    # S-JTSK (Ferro) - S-JTSK (Ferro) / Krovak East North
    (5221, 24.830160, 59.755896, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / Gabon TM
    (5223, 7.497381, -4.509566, 1e-4, 0.0, 0.0, 0.000163568736752),
    # S-JTSK/05
    (5228, 0.002224, 0.003750, 1e-4, 0.0, 0.0, 0.00671925487742),
    # S-JTSK/05 (Ferro)
    (5229, -17.662918, 0.003348, 1e-4, 0.0, 0.0, 0.00817180656864),
    # SLD99
    (5233, 0.007855, 0.000319, 1e-4, 0.0, 0.0, 0.00335802855308),
    # Kandawala - Kandawala / Sri Lanka Grid
    (5234, 78.969868, 5.189547, 1e-4, 0.0, 0.0, 1.10820110422e-06),
    # SLD99 - SLD99 / Sri Lanka Grid 1999
    (5235, 76.282242, 2.471189, 1e-4, 0.0, 0.0, 0.00408819635049),
    # ETRS89 - ETRS89 / LCC Germany (E-N)
    (5243, 10.500000, 51.0, 1e-4, 0.0, 0.0, 1.57945152274e-06),
    # GDBD2009
    (5246, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # GDBD2009 - GDBD2009 / Brunei BRSO
    (5247, 109.685821, -0.000173, 1e-4, 0.0, 0.0, 1e-07),
    # TUREF
    (5252, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # TUREF - TUREF / TM27
    (5253, 22.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # TUREF - TUREF / TM30
    (5254, 25.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405116372),
    # TUREF - TUREF / TM33
    (5255, 28.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405116372),
    # TUREF - TUREF / TM36
    (5256, 31.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # TUREF - TUREF / TM39
    (5257, 34.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # TUREF - TUREF / TM42
    (5258, 37.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # TUREF - TUREF / TM45
    (5259, 40.513048, 0.0, 1e-4, 0.0, 0.0, 0.00016740616411),
    # DRUKREF 03
    (5264, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # DRUKREF 03 - DRUKREF 03 / Bhutan National Grid
    (5266, 87.754790, 0.0, 1e-4, 0.0, 0.0, 3.77386459149e-06),
    # TUREF - TUREF / 3-degree Gauss-Kruger zone 9
    (5269, -32.937314, 0.0, 1e-4, 0.0, 0.0, 1736443),
    # TUREF - TUREF / 3-degree Gauss-Kruger zone 10
    (5270, -27.806557, 0.0, 1e-4, 0.0, 0.0, 3901108),
    # TUREF - TUREF / 3-degree Gauss-Kruger zone 11
    (5271, -16.856353, 0.0, 1e-4, 0.0, 0.0, 7621009),
    # TUREF - TUREF / 3-degree Gauss-Kruger zone 12
    (5272, 3.503904, 0.0, 1e-4, 0.0, 0.0, 13004933.5289),
    # TUREF - TUREF / 3-degree Gauss-Kruger zone 13
    (5273, 38.628964, 0.0, 1e-4, 0.0, 0.0, 20188044.2593),
    # TUREF - TUREF / 3-degree Gauss-Kruger zone 14
    (5274, 96.198090, 0.0, 1e-4, 0.0, 0.0, 32544772.6287),
    # TUREF - TUREF / 3-degree Gauss-Kruger zone 15
    (5275, -173.122818, 0.0, 1e-4, 0.0, 0.0, 5),
    # DRUKREF 03 - DRUKREF 03 / Bumthang TM
    (5292, 88.302829, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Chhukha TM
    (5293, 87.119496, 22.579430, 1e-4, 0.0, 0.0, 1.18346360978e-06),
    # DRUKREF 03 - DRUKREF 03 / Dagana TM
    (5294, 87.419496, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Gasa TM
    (5295, 87.602829, 22.579430, 1e-4, 0.0, 0.0, 1.18346360978e-06),
    # DRUKREF 03 - DRUKREF 03 / Ha TM
    (5296, 87.719496, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Lhuentse TM
    (5297, 88.702829, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Mongar TM
    (5298, 88.802829, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Paro TM
    (5299, 86.919496, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Pemagatshel TM
    (5300, 88.919496, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Punakha TM
    (5301, 87.419496, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Samdrup Jongkhar TM
    (5302, 89.136162, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Samtse TM
    (5303, 86.636162, 22.579430, 1e-4, 0.0, 0.0, 1.18346360978e-06),
    # DRUKREF 03 - DRUKREF 03 / Sarpang TM
    (5304, 87.836162, 22.579430, 1e-4, 0.0, 0.0, 1.18346360978e-06),
    # DRUKREF 03 - DRUKREF 03 / Thimphu TM
    (5305, 87.119496, 22.579430, 1e-4, 0.0, 0.0, 1.18346360978e-06),
    # DRUKREF 03 - DRUKREF 03 / Trashigang TM
    (5306, 89.319496, 22.579430, 1e-4, 0.0, 0.0, 1.18346360978e-06),
    # DRUKREF 03 - DRUKREF 03 / Trongsa TM
    (5307, 88.069496, 22.579430, 1e-4, 0.0, 0.0, 1.18346360978e-06),
    # DRUKREF 03 - DRUKREF 03 / Tsirang TM
    (5308, 87.736162, 22.579430, 1e-4, 0.0, 0.0, 1.18346360978e-06),
    # DRUKREF 03 - DRUKREF 03 / Wangdue Phodrang TM
    (5309, 87.686162, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Yangtse TM
    (5310, 89.136162, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # DRUKREF 03 - DRUKREF 03 / Zhemgang TM
    (5311, 88.436162, 22.579430, 1e-4, 0.0, 0.0, 1.18154275697e-06),
    # ETRS89 - ETRS89 / Faroe TM
    (5316, -10.056807, 54.087859, 1e-4, 0.0, 0.0, 1.38898176374e-05),
    # NAD83 - NAD83 / Teranet Ontario Lambert
    (5320, -90.494818, -0.282516, 1e-4, 0.0, 0.0, 9.45720646461e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Teranet Ontario Lambert
    (5321, -90.494818, -0.282516, 1e-4, 0.0, 0.0, 9.45720646461e-07),
    # ISN2004
    (5324, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # ISN2004 - ISN2004 / Lambert 2004
    (5325, -49.212704, 58.603837, 1e-4, 0.0, 0.0, 4.32541128248e-07),
    # Segara (Jakarta) - Segara (Jakarta) / NEIEZ
    (5329, 74.861360, -8.135836, 1e-4, 0.0, 0.0, 1e-07),
    # Batavia (Jakarta) - Batavia (Jakarta) / NEIEZ
    (5330, 74.861125, -8.136645, 1e-4, 0.0, 0.0, 1e-07),
    # Makassar (Jakarta) - Makassar (Jakarta) / NEIEZ
    (5331, 74.862589, -8.135162, 1e-4, 0.0, 0.0, 1e-07),
    # Aratu - Aratu / UTM zone 25S
    (5337, 117.901581, -89.997092, 1e-4, 0.0, 0.0, 752569),
    # POSGAR 2007
    (5340, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # POSGAR 2007 - POSGAR 2007 / Argentina 1
    (5343, -72.0, 90.0, 1e-4, 0.0, 0.0, 32255897.1877),
    # POSGAR 2007 - POSGAR 2007 / Argentina 2
    (5344, -69.0, 90.0, 1e-4, 0.0, 0.0, 33755897.1877),
    # POSGAR 2007 - POSGAR 2007 / Argentina 3
    (5345, -66.0, 90.0, 1e-4, 0.0, 0.0, 35255897.1877),
    # POSGAR 2007 - POSGAR 2007 / Argentina 4
    (5346, -63.0, 90.0, 1e-4, 0.0, 0.0, 36755897.1877),
    # POSGAR 2007 - POSGAR 2007 / Argentina 5
    (5347, -60.0, 90.0, 1e-4, 0.0, 0.0, 38255897.1877),
    # POSGAR 2007 - POSGAR 2007 / Argentina 6
    (5348, -57.0, 90.0, 1e-4, 0.0, 0.0, 39755897.1877),
    # POSGAR 2007 - POSGAR 2007 / Argentina 7
    (5349, -54.0, 90.0, 1e-4, 0.0, 0.0, 41255897.1877),
    # MARGEN
    (5354, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # MARGEN - MARGEN / UTM zone 20S
    (5355, -63.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # MARGEN - MARGEN / UTM zone 19S
    (5356, -69.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # MARGEN - MARGEN / UTM zone 21S
    (5357, -57.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS-Chile
    (5360, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # SIRGAS-Chile - SIRGAS-Chile / UTM zone 19S
    (5361, -69.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS-Chile - SIRGAS-Chile / UTM zone 18S
    (5362, -75.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # CR05
    (5365, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # CR05 - CR05 / CRTM05
    (5367, -88.487400, 0.0, 1e-4, 0.0, 0.0, 0.000167486316059),
    # MACARIO SOLIS
    (5371, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Peru96
    (5373, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # SIRGAS-ROU98
    (5381, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # SIRGAS-ROU98 - SIRGAS-ROU98 / UTM zone 21S
    (5382, -57.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS-ROU98 - SIRGAS-ROU98 / UTM zone 22S
    (5383, -51.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Peru96 - Peru96 / UTM zone 18S
    (5387, -75.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Peru96 - Peru96 / UTM zone 17S (deprecated)
    (5388, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Peru96 - Peru96 / UTM zone 19S
    (5389, -69.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS_ES2007.8
    (5393, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 26S
    (5396, -27.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Ocotepeque 1935
    (5451, 0.000862, -0.000886, 8e-4, 0.0, 0.0, 1e-07),
    # Ocotepeque 1935 - Ocotepeque 1935 / Costa Rica Norte
    (5456, -88.862685, 7.975734, 1e-4, 0.0, 0.0, 1e-07),
    # Ocotepeque 1935 - Ocotepeque 1935 / Costa Rica Sur
    (5457, -88.175159, 6.006250, 2e-4, 0.0, 0.0, 1e-07),
    # NAD27 - Ocotepeque 1935 / Guatemala Norte (deprecated)
    (5458, -94.959657, 14.123746, 1e-4, 0.0, 0.0, 1.61264324561e-07),
    # Ocotepeque 1935 - Ocotepeque 1935 / Guatemala Sur
    (5459, -94.915777, 11.906278, 2e-4, 0.0, 0.0, 1.52620486915e-07),
    # Ocotepeque 1935 - Ocotepeque 1935 / El Salvador Lambert
    (5460, -93.569589, 11.065684, 2e-4, 0.0, 0.0, 1.52009306476e-07),
    # Ocotepeque 1935 - Ocotepeque 1935 / Nicaragua Norte
    (5461, -90.059938, 10.570065, 1e-4, 0.0, 0.0, 1e-07),
    # Ocotepeque 1935 - Ocotepeque 1935 / Nicaragua Sur
    (5462, -90.042093, 9.084395, 1e-4, 0.0, 0.0, 1e-07),
    # SAD69 - SAD69 / UTM zone 17N
    (5463, -85.489323, -0.000348, 1e-4, 0.0, 0.0, 0.000167732447153),
    # Sibun Gorge 1922
    (5464, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Sibun Gorge 1922 - Sibun Gorge 1922 / Colony Grid (deprecated)
    (5466, -89.250000, 15.833333, 1e-4, 0.0, 0.0, 1e-07),
    # Panama-Colon 1911
    (5467, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Panama-Colon 1911 - Panama-Colon 1911 / Panama Lambert
    (5469, -84.509509, 5.724906, 1e-4, 0.0, 0.0, 1e-07),
    # Panama-Colon 1911 - Panama-Colon 1911 / Panama Polyconic
    (5472, -89.214794, -0.780239, 1e-4, 0.0, 0.0, 1e-07),
    # RSRGD2000 - RSRGD2000 / MSLC2000
    (5479, 42.909280, -25.034151, 1e-4, 0.0, 0.0, 2.20723450184e-07),
    # RSRGD2000 - RSRGD2000 / BCLC2000
    (5480, 57.310896, -45.235313, 1e-4, 0.0, 0.0, 1e-07),
    # RSRGD2000 - RSRGD2000 / PCLC2000
    (5481, 93.043203, -61.951855, 1e-4, 0.0, 0.0, 1.71305146068e-07),
    # RSRGD2000 - RSRGD2000 / RSPS2000
    (5482, 78.690068, -46.245086, 1e-4, 0.0, 0.0, 1e-07),
    # RGAF09
    (5489, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGAF09 - RGAF09 / UTM zone 20N
    (5490, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # S-JTSK - S-JTSK / Krovak
    (5513, 24.830160, 59.755896, 1e-4, 0.0, 0.0, 1e-07),
    # S-JTSK - S-JTSK / Krovak East North
    (5514, 24.830160, 59.755896, 1e-4, 0.0, 0.0, 1e-07),
    # Chatham Islands 1971 - CI1971 / Chatham Islands Map Grid
    (5518, 178.643654, -49.744587, 1e-4, 0.0, 0.0, 0.000230039499002),
    # Chatham Islands 1979 - CI1979 / Chatham Islands Map Grid
    (5519, 178.643635, -49.744581, 1e-4, 0.0, 0.0, 0.000290793715976),
    # DHDN - DHDN / 3-degree Gauss-Kruger zone 1
    (5520, -10.351972, 0.003760, 1e-4, 0.0, 0.0, 0.292022259121),
    # WGS 84 - WGS 84 / Gabon TM 2011
    (5523, -8.603553, -47.862028, 1e-4, 0.0, 0.0, 40),
    # Corrego Alegre 1961
    (5524, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # SAD69(96)
    (5527, 0.000035, -0.000346, 1e-4, 0.0, 0.0, 1e-07),
    # SAD69(96) - SAD69(96) / UTM zone 21S
    (5531, 176.702863, -89.999396, 1e-4, 0.0, 0.0, 753000),
    # SAD69 - SAD69(96) / UTM zone 22S (deprecated)
    (5532, 176.260999, -89.999400, 3, 0.0, 0.0, 753000),
    # SAD69(96) - SAD69(96) / UTM zone 23S
    (5533, 176.702863, -89.999396, 1e-4, 0.0, 0.0, 753000),
    # SAD69(96) - SAD69(96) / UTM zone 24S
    (5534, 176.702863, -89.999396, 1e-4, 0.0, 0.0, 753000),
    # SAD69(96) - SAD69(96) / UTM zone 25S
    (5535, 176.702863, -89.999396, 1e-4, 0.0, 0.0, 753000),
    # Corrego Alegre 1961 - Corrego Alegre 1961 / UTM zone 21S
    (5536, -57.0, -90.0, 1e-4, 0.0, 0.0, 752569),
    # Corrego Alegre 1961 - Corrego Alegre 1961 / UTM zone 22S
    (5537, -51.0, -90.0, 1e-4, 0.0, 0.0, 752569),
    # Corrego Alegre 1961 - Corrego Alegre 1961 / UTM zone 23S
    (5538, -45.0, -90.0, 1e-4, 0.0, 0.0, 752569),
    # Corrego Alegre 1961 - Corrego Alegre 1961 / UTM zone 24S
    (5539, -39.0, -90.0, 1e-4, 0.0, 0.0, 752569),
    # PNG94
    (5546, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # PNG94 - PNG94 / PNGMG94 zone 54
    (5550, 141.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # PNG94 - PNG94 / PNGMG94 zone 55
    (5551, 147.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # PNG94 - PNG94 / PNGMG94 zone 56
    (5552, 153.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Ocotepeque 1935 - Ocotepeque 1935 / Guatemala Norte
    (5559, -94.957842, 14.122123, 2e-4, 0.0, 0.0, 1.67638063431e-07),
    # UCS-2000
    (5561, -0.001062, -0.000808, 1e-4, 0.0, 0.0, 0.000149358529598),
    # UCS-2000 - UCS-2000 / Gauss-Kruger zone 4
    (5562, -16.408652, -0.000804, 1e-4, 0.0, 0.0, 2788),
    # UCS-2000 - UCS-2000 / Gauss-Kruger zone 5
    (5563, -17.144868, -0.000803, 1e-4, 0.0, 0.0, 15649),
    # UCS-2000 - UCS-2000 / Gauss-Kruger zone 6
    (5564, -17.069343, -0.000803, 1e-4, 0.0, 0.0, 66222),
    # UCS-2000 - UCS-2000 / Gauss-Kruger zone 7
    (5565, -15.997802, -0.000804, 1e-4, 0.0, 0.0, 228895),
    # UCS-2000 - UCS-2000 / Gauss-Kruger CM 21E
    (5566, 16.512050, -0.000804, 1e-4, 0.0, 0.0, 0.000323477549767),
    # UCS-2000 - UCS-2000 / Gauss-Kruger CM 27E
    (5567, 22.512072, -0.000800, 1e-4, 0.0, 0.0, 0.000325146674106),
    # UCS-2000 - UCS-2000 / Gauss-Kruger CM 33E
    (5568, 28.512108, -0.000796, 1e-4, 0.0, 0.0, 0.000326583209941),
    # UCS-2000 - UCS-2000 / Gauss-Kruger CM 39E
    (5569, 34.512157, -0.000791, 1e-4, 0.0, 0.0, 0.000327894830329),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger zone 7 (deprecated)
    (5570, -33.997570, -0.000791, 1e-4, 0.0, 0.0, 228895),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger zone 8 (deprecated)
    (5571, -34.542207, -0.000791, 1e-4, 0.0, 0.0, 675426),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger zone 9 (deprecated)
    (5572, -32.938072, -0.000792, 1e-4, 0.0, 0.0, 1736227),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger zone 10 (deprecated)
    (5573, -27.808187, -0.000796, 1e-4, 0.0, 0.0, 3900661),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger zone 11 (deprecated)
    (5574, -16.859619, -0.000804, 1e-4, 0.0, 0.0, 7620250),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger zone 12 (deprecated)
    (5575, 3.497831, -0.000808, 1e-4, 0.0, 0.0, 13003845.6945),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger zone 13 (deprecated)
    (5576, 38.618505, -0.000786, 1e-4, 0.0, 0.0, 20186451.1939),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger CM 21E (deprecated)
    (5577, 16.512050, -0.000804, 1e-4, 0.0, 0.0, 0.000323477549767),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger CM 24E (deprecated)
    (5578, 19.512059, -0.000802, 1e-4, 0.0, 0.0, 0.000324354820486),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger CM 27E (deprecated)
    (5579, 22.512072, -0.000800, 1e-4, 0.0, 0.0, 0.000325146674106),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger CM 30E (deprecated)
    (5580, 25.512088, -0.000798, 1e-4, 0.0, 0.0, 0.000325891165461),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger CM 33E (deprecated)
    (5581, 28.512108, -0.000796, 1e-4, 0.0, 0.0, 0.000326583209941),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger CM 36E (deprecated)
    (5582, 31.512131, -0.000793, 1e-4, 0.0, 0.0, 0.000327243842434),
    # UCS-2000 - UCS-2000 / 3-degree Gauss-Kruger CM 39E (deprecated)
    (5583, 34.512157, -0.000791, 1e-4, 0.0, 0.0, 0.000327894830329),
    # NAD27 - NAD27 / New Brunswick Stereographic (NAD27)
    (5588, -70.279455, 43.693533, 6e-4, 0.0, 0.0, 1e-07),
    # Sibun Gorge 1922 - Sibun Gorge 1922 / Colony Grid
    (5589, -89.250000, 15.833333, 1e-4, 0.0, 0.0, 1e-07),
    # FEH2010
    (5593, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # FEH2010 - FEH2010 / Fehmarnbelt TM
    (5596, 2.387006, 0.0, 1e-4, 0.0, 0.0, 0.0141003574245),
    # NAD27 - NAD27 / Michigan East
    (5623, -85.491514, 41.485521, 1e-4, 0.0, 0.0, 2.77130146581e-06),
    # NAD27 - NAD27 / Michigan Old Central
    (5624, -87.574909, 41.485520, 1e-4, 0.0, 0.0, 2.77853742649e-06),
    # NAD27 - NAD27 / Michigan West
    (5625, -90.574909, 41.485520, 2e-4, 0.0, 0.0, 2.7734918395e-06),
    # ED50 - ED50 / TM 6 NE
    (5627, 1.510573, -0.001094, 1e-4, 0.0, 0.0, 0.000168836500961),
    # Moznet - Moznet / UTM zone 38S
    (5629, 45.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Gauss-Kruger zone 2 (E-N)
    (5631, -12.900861, -0.000653, 1e-4, 0.0, 0.0, 19),
    # PTRA08 - PTRA08 / LCC Europe
    (5632, -25.397641, 18.366885, 1e-4, 0.0, 0.0, 2.28406861424e-07),
    # PTRA08 - PTRA08 / LAEA Europe
    (5633, -29.086835, 12.993574, 1e-4, 0.0, 0.0, 0.00240807747468),
    # REGCAN95 - REGCAN95 / LCC Europe
    (5634, -25.397641, 18.366885, 1e-4, 0.0, 0.0, 2.28406861424e-07),
    # REGCAN95 - REGCAN95 / LAEA Europe
    (5635, -29.086835, 12.993574, 1e-4, 0.0, 0.0, 0.00240807747468),
    # TUREF - TUREF / LAEA Europe
    (5636, -29.086835, 12.993574, 1e-4, 0.0, 0.0, 0.00240807747468),
    # TUREF - TUREF / LCC Europe
    (5637, -25.397641, 18.366885, 1e-4, 0.0, 0.0, 2.28406861424e-07),
    # ISN2004 - ISN2004 / LAEA Europe
    (5638, -29.086835, 12.993574, 1e-4, 0.0, 0.0, 0.00240807747468),
    # ISN2004 - ISN2004 / LCC Europe
    (5639, -25.397641, 18.366885, 1e-4, 0.0, 0.0, 2.28406861424e-07),
    # SIRGAS 2000 - SIRGAS 2000 / Brazil Mercator
    (5641, -87.942959, -66.608177, 1e-4, 0.0, 0.0, 1e-07),
    # ED50 - ED50 / SPBA LCC
    (5643, -0.791761, 47.445742, 1e-4, 0.0, 0.0, 3.34496088561e-06),
    # RGR92 - RGR92 / UTM zone 39S
    (5644, 51.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # NAD83 - NAD83 / Vermont (ftUS)
    (5646, -78.566435, 42.339184, 1e-4, 0.0, 0.0, 0.00222229043748),
    # ETRS89 - ETRS89 / UTM zone 31N (zE-N)
    (5649, 71.405249, 0.0, 1e-4, 0.0, 0.0, 62783422.6674),
    # ETRS89 - ETRS89 / UTM zone 33N (zE-N)
    (5650, 111.826522, 0.0, 1e-4, 0.0, 0.0, 3),
    # ETRS89 - ETRS89 / UTM zone 31N (N-zE)
    (5651, 71.405249, 0.0, 1e-4, 0.0, 0.0, 62783422.6674),
    # ETRS89 - ETRS89 / UTM zone 32N (N-zE)
    (5652, 11.361590, 0.0, 1e-4, 0.0, 0.0, 49144291.1282),
    # ETRS89 - ETRS89 / UTM zone 33N (N-zE)
    (5653, 111.826522, 0.0, 1e-4, 0.0, 0.0, 3),
    # NAD83(HARN) - NAD83(HARN) / Vermont (ftUS)
    (5654, -78.566435, 42.339184, 1e-4, 0.0, 0.0, 0.00222229043748),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Vermont (ftUS)
    (5655, -78.566435, 42.339184, 1e-4, 0.0, 0.0, 0.00222229043748),
    # Monte Mario - Monte Mario / TM Emilia-Romagna
    (5659, 3.453244, 36.014522, 1e-4, 0.0, 0.0, 0.00284557038685),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / Gauss-Kruger zone 3 (E-N)
    (5663, -14.962310, -0.000650, 1e-4, 0.0, 0.0, 325),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / Gauss-Kruger zone 2 (E-N)
    (5664, -12.900886, -0.000705, 1e-4, 0.0, 0.0, 19),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / Gauss-Kruger zone 3 (E-N)
    (5665, -14.962339, -0.000705, 1e-4, 0.0, 0.0, 325),
    # PD/83 - PD/83 / 3-degree Gauss-Kruger zone 3 (E-N)
    (5666, -20.964993, 0.0, 1e-4, 0.0, 0.0, 325),
    # PD/83 - PD/83 / 3-degree Gauss-Kruger zone 4 (E-N)
    (5667, -25.412011, 0.0, 1e-4, 0.0, 0.0, 2790),
    # RD/83 - RD/83 / 3-degree Gauss-Kruger zone 4 (E-N)
    (5668, -25.412011, 0.0, 1e-4, 0.0, 0.0, 2790),
    # RD/83 - RD/83 / 3-degree Gauss-Kruger zone 5 (E-N)
    (5669, -29.148663, 0.0, 1e-4, 0.0, 0.0, 15664),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 3 (E-N)
    (5670, -20.962237, -0.000640, 1e-4, 0.0, 0.0, 325),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 4 (E-N)
    (5671, -25.408531, -0.000634, 1e-4, 0.0, 0.0, 2788),
    # Pulkovo 1942(58) - Pulkovo 1942(58) / 3-degree Gauss-Kruger zone 5 (E-N)
    (5672, -29.144697, -0.000628, 1e-4, 0.0, 0.0, 15649),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 3 (E-N)
    (5673, -20.962281, -0.000705, 1e-4, 0.0, 0.0, 325),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 4 (E-N)
    (5674, -25.408587, -0.000705, 1e-4, 0.0, 0.0, 2788),
    # Pulkovo 1942(83) - Pulkovo 1942(83) / 3-degree Gauss-Kruger zone 5 (E-N)
    (5675, -29.144763, -0.000705, 1e-4, 0.0, 0.0, 15649),
    # DHDN - DHDN / 3-degree Gauss-Kruger zone 2 (E-N)
    (5676, -15.901231, 0.003755, 1e-4, 0.0, 0.0, 19),
    # DHDN - DHDN / 3-degree Gauss-Kruger zone 3 (E-N)
    (5677, -20.963134, 0.003750, 1e-4, 0.0, 0.0, 325),
    # DHDN - DHDN / 3-degree Gauss-Kruger zone 4 (E-N)
    (5678, -25.409789, 0.003747, 1e-4, 0.0, 0.0, 2790),
    # DHDN - DHDN / 3-degree Gauss-Kruger zone 5 (E-N)
    (5679, -29.146150, 0.003744, 1e-4, 0.0, 0.0, 15664),
    # DHDN - DHDN / 3-degree Gauss-Kruger zone 1 (E-N)
    (5680, -10.351972, 0.003760, 1e-4, 0.0, 0.0, 0.292022259121),
    # DB_REF
    (5681, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # DB_REF - DB_REF / 3-degree Gauss-Kruger zone 2 (E-N)
    (5682, -15.902658, 0.0, 1e-4, 0.0, 0.0, 19),
    # DB_REF - DB_REF / 3-degree Gauss-Kruger zone 3 (E-N)
    (5683, -20.964993, 0.0, 1e-4, 0.0, 0.0, 325),
    # DB_REF - DB_REF / 3-degree Gauss-Kruger zone 4 (E-N)
    (5684, -25.412011, 0.0, 1e-4, 0.0, 0.0, 2790),
    # DB_REF - DB_REF / 3-degree Gauss-Kruger zone 5 (E-N)
    (5685, -29.148663, 0.0, 1e-4, 0.0, 0.0, 15664),
    # NZGD2000 - NZGD2000 / UTM zone 1S
    (5700, -177.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # AGD66 - AGD66 / ACT Standard Grid
    (5825, 146.644570, -40.697039, 1e-4, 0.0, 0.0, 0.000100726771052),
    # Yemen NGN96 - Yemen NGN96 / UTM zone 37N
    (5836, 34.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # Yemen NGN96 - Yemen NGN96 / UTM zone 40N
    (5837, 52.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # Peru96 - Peru96 / UTM zone 17S
    (5839, -81.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / TM 12 SE
    (5842, 12.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGRDC 2005 - RGRDC 2005 / Congo TM zone 30
    (5844, -180.0, -1745366112639232000.0, 1e-4, 0.0, 0.0, 4.56935779774e+16),
    # SAD69(96) - SAD69(96) / UTM zone 22S
    (5858, 176.702863, -89.999396, 1e-4, 0.0, 0.0, 753000),
    # SAD69(96) - SAD69(96) / UTM zone 18S
    (5875, 176.702863, -89.999396, 1e-4, 0.0, 0.0, 753000),
    # SAD69(96) - SAD69(96) / UTM zone 19S
    (5876, 176.702863, -89.999396, 1e-4, 0.0, 0.0, 753000),
    # SAD69(96) - SAD69(96) / UTM zone 20S
    (5877, 176.702863, -89.999396, 1e-4, 0.0, 0.0, 753000),
    # Cadastre 1997 - Cadastre 1997 / UTM zone 38S
    (5879, -171.435069, -89.996544, 1e-4, 0.0, 0.0, 752569),
    # TGD2005
    (5886, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # TGD2005 - TGD2005 / Tonga Map Grid
    (5887, 164.421674, -43.615403, 1e-4, 0.0, 0.0, 16),
    # Unspecified Hughes 1980 ellipsoid - JAXA Snow Depth Polar Stereographic N
    (5890, 90.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone A1
    (5921, -111.0, 81.317226, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone A2
    (5922, -39.0, 81.317226, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone A3
    (5923, 33.0, 81.317226, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone A4
    (5924, 105.0, 81.317226, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone A5
    (5925, 177.0, 81.317226, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone B1
    (5926, -111.0, 73.155741, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone B2
    (5927, -39.0, 73.155741, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone B3
    (5928, 33.0, 73.155741, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone B4
    (5929, 105.0, 73.155741, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone B5
    (5930, 177.0, 73.155741, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone C1
    (5931, -111.0, 65.101271, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone C2
    (5932, -39.0, 65.101271, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone C3
    (5933, 33.0, 65.101271, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone C4
    (5934, 105.0, 65.101271, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic Regional zone C5
    (5935, 177.0, 65.101271, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Alaska Polar Stereographic
    (5936, 165.0, 64.916412, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Canada Polar Stereographic
    (5937, -145.0, 64.916412, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Greenland Polar Stereographic
    (5938, -78.0, 64.916412, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Norway Polar Stereographic
    (5939, -27.0, 64.916412, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Russia Polar Stereographic
    (5940, 60.0, 64.916412, 1e-4, 0.0, 0.0, 1e-07),
    # GR96 - GR96 / EPSG Arctic zone 1-25
    (6050, -115.763772, -37.842390, 1e-4, 0.0, 0.0, 2.15135514736e-07),
    # GR96 - GR96 / EPSG Arctic zone 2-18
    (6051, -132.380091, -23.354913, 1e-4, 0.0, 0.0, 4.85451892018e-07),
    # GR96 - GR96 / EPSG Arctic zone 2-20
    (6052, -93.384435, -28.562436, 1e-4, 0.0, 0.0, 4.20492142439e-07),
    # GR96 - GR96 / EPSG Arctic zone 3-29
    (6053, -151.396642, -46.240968, 1e-4, 0.0, 0.0, 1e-07),
    # GR96 - GR96 / EPSG Arctic zone 3-31
    (6054, -121.982960, -48.774847, 1e-4, 0.0, 0.0, 1.9506085664e-05),
    # GR96 - GR96 / EPSG Arctic zone 3-33
    (6055, -93.500755, -51.047691, 1e-4, 0.0, 0.0, 1.46285165101e-05),
    # GR96 - GR96 / EPSG Arctic zone 4-20
    (6056, -139.705571, -32.193421, 1e-4, 0.0, 0.0, 3.632158041e-07),
    # GR96 - GR96 / EPSG Arctic zone 4-22
    (6057, -116.168778, -36.416951, 1e-4, 0.0, 0.0, 2.57045030594e-07),
    # GR96 - GR96 / EPSG Arctic zone 4-24
    (6058, -92.408598, -40.133686, 1e-4, 0.0, 0.0, 1.78813934326e-07),
    # GR96 - GR96 / EPSG Arctic zone 5-41
    (6059, -145.743452, -60.509871, 1e-4, 0.0, 0.0, 2.94484198093e-06),
    # GR96 - GR96 / EPSG Arctic zone 5-43
    (6060, -126.233362, -61.836602, 1e-4, 0.0, 0.0, 2.22260132432e-06),
    # GR96 - GR96 / EPSG Arctic zone 5-45
    (6061, -106.681418, -63.055400, 1e-4, 0.0, 0.0, 1.71130523086e-06),
    # GR96 - GR96 / EPSG Arctic zone 6-26
    (6062, -132.482424, -47.160117, 1e-4, 0.0, 0.0, 1e-07),
    # GR96 - GR96 / EPSG Arctic zone 6-28
    (6063, -115.805377, -49.845064, 1e-4, 0.0, 0.0, 1.64075754583e-05),
    # GR96 - GR96 / EPSG Arctic zone 6-30
    (6064, -98.969642, -52.249669, 1e-4, 0.0, 0.0, 1.18114985526e-05),
    # GR96 - GR96 / EPSG Arctic zone 7-11
    (6065, -103.573982, -18.061038, 1e-4, 0.0, 0.0, 5.0151720643e-07),
    # GR96 - GR96 / EPSG Arctic zone 7-13
    (6066, -91.544416, -23.641678, 1e-4, 0.0, 0.0, 4.90341335535e-07),
    # GR96 - GR96 / EPSG Arctic zone 8-20
    (6067, -119.800191, -42.498574, 1e-4, 0.0, 0.0, 1.50874257088e-07),
    # GR96 - GR96 / EPSG Arctic zone 8-22
    (6068, -107.359925, -45.823712, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / EPSG Arctic zone 2-22
    (6069, -66.214602, -33.094516, 1e-4, 0.0, 0.0, 3.1222589314e-07),
    # ETRS89 - ETRS89 / EPSG Arctic zone 3-11
    (6070, -47.773014, -0.855411, 1e-4, 0.0, 0.0, 4.77069988847e-06),
    # ETRS89 - ETRS89 / EPSG Arctic zone 4-26
    (6071, -69.471682, -43.418099, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / EPSG Arctic zone 4-28
    (6072, -46.392746, -46.333600, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / EPSG Arctic zone 5-11
    (6073, -45.534021, -8.736516, 1e-4, 0.0, 0.0, 3.35276126862e-07),
    # ETRS89 - ETRS89 / EPSG Arctic zone 5-13
    (6074, -29.818529, -15.665337, 1e-4, 0.0, 0.0, 5.11296093464e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 2-24
    (6075, -29.912017, -37.059904, 1e-4, 0.0, 0.0, 2.43075191975e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 2-26
    (6076, 9.493997, -40.548901, 1e-4, 0.0, 0.0, 1.65542587638e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 3-13
    (6077, -19.904901, -9.036614, 1e-4, 0.0, 0.0, 3.4854747355e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 3-15
    (6078, 8.691015, -16.177716, 1e-4, 0.0, 0.0, 4.92436811328e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 3-17
    (6079, 37.793387, -22.383588, 1e-4, 0.0, 0.0, 5.11994585395e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 3-19
    (6080, 67.260428, -27.777285, 1e-4, 0.0, 0.0, 4.47733327746e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 4-30
    (6081, -23.198099, -48.933574, 1e-4, 0.0, 0.0, 1.90646387637e-05),
    # WGS 84 - WGS 84 / EPSG Arctic zone 4-32
    (6082, 0.091993, -51.262749, 1e-4, 0.0, 0.0, 1.41179189086e-05),
    # WGS 84 - WGS 84 / EPSG Arctic zone 4-34
    (6083, 23.461683, -53.358557, 1e-4, 0.0, 0.0, 1.04955397546e-05),
    # WGS 84 - WGS 84 / EPSG Arctic zone 4-36
    (6084, 46.898418, -55.252363, 1e-4, 0.0, 0.0, 7.75465741754e-06),
    # WGS 84 - WGS 84 / EPSG Arctic zone 4-38
    (6085, 70.392126, -56.970518, 1e-4, 0.0, 0.0, 5.83939254284e-06),
    # WGS 84 - WGS 84 / EPSG Arctic zone 4-40
    (6086, 94.934645, -58.535241, 1e-4, 0.0, 0.0, 4.39630821347e-06),
    # WGS 84 - WGS 84 / EPSG Arctic zone 5-15
    (6087, -13.237391, -21.879896, 1e-4, 0.0, 0.0, 5.11296093464e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 5-17
    (6088, 3.987746, -27.390578, 1e-4, 0.0, 0.0, 4.63798642159e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 5-19
    (6089, 22.699638, -32.252901, 1e-4, 0.0, 0.0, 3.52039933205e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 5-21
    (6090, 41.785703, -36.538719, 1e-4, 0.0, 0.0, 2.41678208113e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 5-23
    (6091, 61.164168, -40.321460, 1e-4, 0.0, 0.0, 1.69035047293e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 5-25
    (6092, 80.774676, -43.669345, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 5-27
    (6093, 100.571953, -46.642840, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / EPSG Arctic zone 5-29
    (6094, 117.521505, -49.294212, 1e-4, 0.0, 0.0, 1.7979182303e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / EPSG Arctic zone 5-31
    (6095, 132.596669, -51.668039, 1e-4, 0.0, 0.0, 1.31609849632e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / EPSG Arctic zone 6-14
    (6096, 132.540377, -22.453391, 1e-4, 0.0, 0.0, 5.1548704505e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / EPSG Arctic zone 6-16
    (6097, 147.088269, -27.853758, 1e-4, 0.0, 0.0, 4.55416738987e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 1-23
    (6098, -175.380682, -34.052014, 1e-4, 0.0, 0.0, 3.02447006106e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 2-14
    (6099, 167.423673, -10.380761, 1e-4, 0.0, 0.0, 3.82773578167e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 2-16
    (6100, -154.141180, -17.343291, 1e-4, 0.0, 0.0, 5.02215698361e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 3-25
    (6101, 150.043539, -40.206809, 1e-4, 0.0, 0.0, 1.91386789083e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 3-27
    (6102, 179.272618, -43.402473, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 3-29
    (6103, -151.396642, -46.240968, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 4-14
    (6104, 161.805903, -15.591682, 1e-4, 0.0, 0.0, 5.02914190292e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 4-16
    (6105, -175.827425, -21.875247, 1e-4, 0.0, 0.0, 5.36441802979e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 4-18
    (6106, -152.955010, -27.377555, 1e-4, 0.0, 0.0, 4.74974513054e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 5-33
    (6107, 147.776553, -53.802099, 1e-4, 0.0, 0.0, 9.67271625996e-06),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 5-35
    (6108, 167.044583, -55.728320, 1e-4, 0.0, 0.0, 7.07991421223e-06),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 5-37
    (6109, -173.612544, -57.473695, 1e-4, 0.0, 0.0, 5.25685027242e-06),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 5-39
    (6110, -154.205611, -59.061078, 1e-4, 0.0, 0.0, 3.88361513615e-06),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 6-18
    (6111, 159.225246, -32.678297, 1e-4, 0.0, 0.0, 3.46451997757e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 6-20
    (6112, 175.822131, -36.966476, 1e-4, 0.0, 0.0, 2.59838998318e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 6-22
    (6113, -167.217805, -40.772147, 1e-4, 0.0, 0.0, 1.7462298274e-07),
    # NAD83(CSRS) - NAD83(CSRS) / EPSG Arctic zone 6-24
    (6114, -149.967483, -44.152048, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 1-27
    (6115, -56.091460, -41.183650, 1e-4, 0.0, 0.0, 1.78115442395e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 1-29
    (6116, 3.625063, -44.145995, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 1-31
    (6117, 63.377426, -46.786825, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 1-21
    (6118, 125.073111, -29.726536, 1e-4, 0.0, 0.0, 4.04426828027e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 2-28
    (6119, 48.982122, -43.635891, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 2-10
    (6120, 93.206348, 7.002579, 1e-4, 0.0, 0.0, 2.06055119634e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 2-12
    (6121, 130.459040, -2.311725, 1e-4, 0.0, 0.0, 1.26399099827e-05),
    # WGS 84 - WGS 84 / EPSG Arctic zone 3-21
    (6122, 96.997854, -32.477658, 1e-4, 0.0, 0.0, 3.51341441274e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 3-23
    (6123, 126.940849, -36.590597, 1e-4, 0.0, 0.0, 2.5425106287e-07),
    # WGS 84 - WGS 84 / EPSG Arctic zone 4-12
    (6124, 139.134310, -8.449851, 1e-4, 0.0, 0.0, 3.18512320518e-07),
    # ETRS89 - ETRS89 / EPSG Arctic zone 5-47
    (6125, -90.092719, -64.178390, 1e-4, 0.0, 0.0, 1.2475065887e-06),
    # GCGD59 - Grand Cayman National Grid 1959
    (6128, -85.488673, 0.001763, 1e-4, 0.0, 0.0, 0.15031818903),
    # SIGD61 - Sister Islands National Grid 1961
    (6129, -85.487830, 0.001791, 1e-4, 0.0, 0.0, 0.0132420083476),
    # CIGD11
    (6135, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # CIGD11 - Cayman Islands National Grid 2011 (deprecated)
    (6141, 72.282665, 13.912814, 1e-4, 0.0, 0.0, 5.33952165072e-07),
    # MGI 1901 - Macedonia State Coordinate System
    # (6204, 16.508591, 0.004341, 1e-4, 0.0, 0.0, 0.000166777958015),
    # Nepal 1981
    # (6207, 0.006524, 0.002219, 1e-4, 0.0, 0.0, 1e-07),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 23N
    # (6210, -49.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 24N
    # (6211, -43.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # NAD83(CORS96) - NAD83(CORS96) / Puerto Rico and Virgin Is.
    # (6307, -68.300715, 16.017453, 1e-4, 0.0, 0.0, 1.52795109898e-07),
    # CGRS93
    # (6311, -0.000076, -0.000051, 1e-4, 0.0, 0.0, 8.6529193709e-06),
    # CGRS93 - CGRS93 / Cyprus Local Transverse Mercator
    # (6312, 30.892364, 31.606673, 1e-4, 0.0, 0.0, 1.01984187495e-05),
    # MGI 1901 - Macedonia State Coordinate System zone 7
    # (6316, -34.002668, 0.004341, 1e-4, 0.0, 0.0, 229299),
    # NAD83(2011)
    # (6318, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(PA11)
    # (6322, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(MA11)
    # (6325, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / UTM zone 59N
    # (6328, 166.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 60N
    # (6329, 172.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 1N
    # (6330, 178.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167734193383),
    # NAD83(2011) - NAD83(2011) / UTM zone 2N
    # (6331, -175.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 3N
    # (6332, -169.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 4N
    # (6333, -163.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 5N
    # (6334, -157.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167734193383),
    # NAD83(2011) - NAD83(2011) / UTM zone 6N
    # (6335, -151.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 7N
    # (6336, -145.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 8N
    # (6337, -139.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 9N
    # (6338, -133.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 10N
    # (6339, -127.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 11N
    # (6340, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 12N
    # (6341, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(2011) - NAD83(2011) / UTM zone 13N
    # (6342, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(2011) - NAD83(2011) / UTM zone 14N
    # (6343, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(2011) - NAD83(2011) / UTM zone 15N
    # (6344, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(2011) - NAD83(2011) / UTM zone 16N
    # (6345, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(2011) - NAD83(2011) / UTM zone 17N
    # (6346, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(2011) - NAD83(2011) / UTM zone 18N
    # (6347, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(2011) - NAD83(2011) / UTM zone 19N
    # (6348, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(2011) - NAD83(2011) / Conus Albers
    # (6350, -96.0, 23.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / EPSG Arctic zone 5-29
    # (6351, 117.521505, -49.294212, 1e-4, 0.0, 0.0, 1.7979182303e-05),
    # NAD83(2011) - NAD83(2011) / EPSG Arctic zone 5-31
    # (6352, 132.596669, -51.668039, 1e-4, 0.0, 0.0, 1.31609849632e-05),
    # NAD83(2011) - NAD83(2011) / EPSG Arctic zone 6-14
    # (6353, 132.540377, -22.453391, 1e-4, 0.0, 0.0, 5.1548704505e-07),
    # NAD83(2011) - NAD83(2011) / EPSG Arctic zone 6-16
    # (6354, 147.088269, -27.853758, 1e-4, 0.0, 0.0, 4.55416738987e-07),
    # NAD83(2011) - NAD83(2011) / Alabama East
    # (6355, -87.916194, 30.483360, 1e-4, 0.0, 0.0, 1.33309797832e-06),
    # NAD83(2011) - NAD83(2011) / Alabama West
    # (6356, -93.703674, 29.853763, 1e-4, 0.0, 0.0, 0.00067773398073),
    # Mexico ITRF92 - Mexico ITRF92 / LCC
    # (6362, -124.445519, 10.258404, 1e-4, 0.0, 0.0, 1e-07),
    # Mexico ITRF2008
    # (6365, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Mexico ITRF2008 - Mexico ITRF2008 / UTM zone 11N
    # (6366, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # Mexico ITRF2008 - Mexico ITRF2008 / UTM zone 12N
    # (6367, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Mexico ITRF2008 - Mexico ITRF2008 / UTM zone 13N
    # (6368, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Mexico ITRF2008 - Mexico ITRF2008 / UTM zone 14N
    # (6369, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Mexico ITRF2008 - Mexico ITRF2008 / UTM zone 15N
    # (6370, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Mexico ITRF2008 - Mexico ITRF2008 / UTM zone 16N
    # (6371, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # Mexico ITRF2008 - Mexico ITRF2008 / LCC
    # (6372, -124.445519, 10.258404, 1e-4, 0.0, 0.0, 1e-07),
    # UCS-2000 - UCS-2000 / Ukraine TM zone 7
    # (6381, 18.305031, -0.000803, 1e-4, 0.0, 0.0, 0.000166513797724),
    # UCS-2000 - UCS-2000 / Ukraine TM zone 8
    # (6382, 21.305042, -0.000801, 1e-4, 0.0, 0.0, 0.00016733884899),
    # UCS-2000 - UCS-2000 / Ukraine TM zone 9
    # (6383, 24.305057, -0.000799, 1e-4, 0.0, 0.0, 0.000169031471808),
    # UCS-2000 - UCS-2000 / Ukraine TM zone 10
    # (6384, 27.305076, -0.000797, 1e-4, 0.0, 0.0, 0.000171528901165),
    # UCS-2000 - UCS-2000 / Ukraine TM zone 11
    # (6385, 30.305097, -0.000794, 1e-4, 0.0, 0.0, 0.000173749190153),
    # UCS-2000 - UCS-2000 / Ukraine TM zone 12
    # (6386, 33.305122, -0.000792, 1e-4, 0.0, 0.0, 0.000175676315435),
    # UCS-2000 - UCS-2000 / Ukraine TM zone 13
    # (6387, 36.305150, -0.000789, 1e-4, 0.0, 0.0, 0.000177321191011),
    # CIGD11 - Cayman Islands National Grid 2011
    # (6391, -88.850668, 13.912814, 1e-4, 0.0, 0.0, 5.2765015454e-07),
    # NAD83(2011) - NAD83(2011) / Alaska Albers
    # (6393, -154.0, 50.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Alaska zone 1
    # (6394, -145.375377, 51.236569, 1e-4, 0.0, 0.0, 1.76858156919e-06),
    # NAD83(2011) - NAD83(2011) / Alaska zone 2
    # (6395, -149.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726326207989),
    # NAD83(2011) - NAD83(2011) / Alaska zone 3
    # (6396, -153.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83(2011) - NAD83(2011) / Alaska zone 4
    # (6397, -157.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83(2011) - NAD83(2011) / Alaska zone 5
    # (6398, -161.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83(2011) - NAD83(2011) / Alaska zone 6
    # (6399, -165.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83(2011) - NAD83(2011) / Alaska zone 7
    # (6400, -169.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726326207989),
    # NAD83(2011) - NAD83(2011) / Alaska zone 8
    # (6401, -173.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726326207989),
    # NAD83(2011) - NAD83(2011) / Alaska zone 9
    # (6402, -177.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83(2011) - NAD83(2011) / Alaska zone 10
    # (6403, 169.941515, 50.118842, 1e-4, 0.0, 0.0, 2.16796352387e-06),
    # NAD83(2011) - NAD83(2011) / Arizona Central
    # (6404, -114.150208, 30.980684, 1e-4, 0.0, 0.0, 2.03625043994e-06),
    # NAD83(2011) - NAD83(2011) / Arizona Central (ft)
    # (6405, -114.150208, 30.980684, 1e-4, 0.0, 0.0, 6.68061167961e-06),
    # NAD83(2011) - NAD83(2011) / Arizona East
    # (6406, -112.400208, 30.980684, 1e-4, 0.0, 0.0, 2.03809133589e-06),
    # NAD83(2011) - NAD83(2011) / Arizona East (ft)
    # (6407, -112.400208, 30.980684, 1e-4, 0.0, 0.0, 6.68665136447e-06),
    # NAD83(2011) - NAD83(2011) / Arizona West
    # (6408, -115.983467, 30.980685, 1e-4, 0.0, 0.0, 2.03845919723e-06),
    # NAD83(2011) - NAD83(2011) / Arizona West (ft)
    # (6409, -115.983467, 30.980685, 1e-4, 0.0, 0.0, 6.68785825863e-06),
    # NAD83(2011) - NAD83(2011) / Arkansas North
    # (6410, -96.343203, 34.253806, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Arkansas North (ftUS)
    # (6411, -96.343203, 34.253806, 1e-4, 0.0, 0.0, 2.06034807436e-07),
    # NAD83(2011) - NAD83(2011) / Arkansas South
    # (6412, -96.090762, 28.992599, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Arkansas South (ftUS)
    # (6413, -96.090762, 28.992599, 1e-4, 0.0, 0.0, 3.15959259751e-07),
    # NAD83(2011) - NAD83(2011) / California Albers
    # (6414, -120.0, 38.016365, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / California zone 1
    # (6415, -143.321156, 32.649532, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / California zone 1 (ftUS)
    # (6416, -143.321156, 32.649532, 1e-4, 0.0, 0.0, 2.79865998891e-07),
    # NAD83(2011) - NAD83(2011) / California zone 2
    # (6417, -142.954019, 31.095993, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / California zone 2 (ftUS)
    # (6418, -142.954019, 31.095993, 1e-4, 0.0, 0.0, 3.30854891217e-07),
    # NAD83(2011) - NAD83(2011) / California zone 3
    # (6419, -141.218751, 30.009900, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / California zone 3 (ftUS)
    # (6420, -141.218751, 30.009900, 1e-4, 0.0, 0.0, 3.35438162438e-07),
    # NAD83(2011) - NAD83(2011) / California zone 4
    # (6421, -139.486472, 28.915572, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / California zone 4 (ftUS)
    # (6422, -139.486472, 28.915572, 1e-4, 0.0, 0.0, 3.79552147933e-07),
    # NAD83(2011) - NAD83(2011) / California zone 5
    # (6423, -138.152186, 27.196010, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / California zone 5 (ftUS)
    # (6424, -138.152186, 27.196010, 1e-4, 0.0, 0.0, 4.19082862209e-07),
    # NAD83(2011) - NAD83(2011) / California zone 6
    # (6425, -136.178771, 25.945490, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / California zone 6 (ftUS)
    # (6426, -136.178771, 25.945490, 1e-4, 0.0, 0.0, 4.50306397397e-07),
    # NAD83(2011) - NAD83(2011) / Colorado Central
    # (6427, -115.464063, 34.638233, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Colorado Central (ftUS)
    # (6428, -115.464063, 34.638233, 1e-4, 0.0, 0.0, 1.92210936802e-07),
    # NAD83(2011) - NAD83(2011) / Colorado North
    # (6429, -115.653598, 36.118325, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Colorado North (ftUS)
    # (6430, -115.653598, 36.118325, 1e-4, 0.0, 0.0, 1.83330848813e-07),
    # NAD83(2011) - NAD83(2011) / Colorado South
    # (6431, -115.330280, 33.489028, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Colorado South (ftUS)
    # (6432, -115.330280, 33.489028, 1e-4, 0.0, 0.0, 2.39189466811e-07),
    # NAD83(2011) - NAD83(2011) / Connecticut
    # (6433, -76.287472, 39.405061, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Connecticut (ftUS)
    # (6434, -76.287472, 39.405061, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Delaware
    # (6435, -77.692913, 37.977968, 1e-4, 0.0, 0.0, 2.82291068248e-06),
    # NAD83(2011) - NAD83(2011) / Delaware (ftUS)
    # (6436, -77.692913, 37.977968, 1e-4, 0.0, 0.0, 9.25010817428e-06),
    # NAD83(2011) - NAD83(2011) / Florida East
    # (6437, -82.970337, 24.320543, 1e-4, 0.0, 0.0, 4.93879810895e-07),
    # NAD83(2011) - NAD83(2011) / Florida East (ftUS)
    # (6438, -82.970337, 24.320543, 1e-4, 0.0, 0.0, 1.6175350876e-06),
    # NAD83(2011) - NAD83(2011) / Florida GDL Albers
    # (6439, -87.929804, 23.942448, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Florida North
    # (6440, -90.650782, 28.853971, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Florida North (ftUS)
    # (6441, -90.650782, 28.853971, 1e-4, 0.0, 0.0, 3.25091364585e-07),
    # NAD83(2011) - NAD83(2011) / Florida West
    # (6442, -83.970337, 24.320543, 1e-4, 0.0, 0.0, 4.93879810895e-07),
    # NAD83(2011) - NAD83(2011) / Florida West (ftUS)
    # (6443, -83.970337, 24.320543, 1e-4, 0.0, 0.0, 1.61132241679e-06),
    # NAD83(2011) - NAD83(2011) / Georgia East
    # (6444, -84.239141, 29.983687, 1e-4, 0.0, 0.0, 1.25848615201e-06),
    # NAD83(2011) - NAD83(2011) / Georgia East (ftUS)
    # (6445, -84.239141, 29.983687, 1e-4, 0.0, 0.0, 4.12373395515e-06),
    # NAD83(2011) - NAD83(2011) / Georgia West
    # (6446, -91.398144, 29.801257, 1e-4, 0.0, 0.0, 0.00142275930678),
    # NAD83(2011) - NAD83(2011) / Georgia West (ftUS)
    # (6447, -91.398144, 29.801257, 1e-4, 0.0, 0.0, 0.00466783311378),
    # NAD83(2011) - NAD83(2011) / Idaho Central
    # (6448, -119.988244, 41.510438, 1e-4, 0.0, 0.0, 0.000560564359692),
    # NAD83(2011) - NAD83(2011) / Idaho Central (ftUS)
    # (6449, -119.988244, 41.510438, 1e-4, 0.0, 0.0, 0.00183911055059),
    # NAD83(2011) - NAD83(2011) / Idaho East
    # (6450, -114.567265, 41.641591, 1e-4, 0.0, 0.0, 3.94830305492e-06),
    # NAD83(2011) - NAD83(2011) / Idaho East (ftUS)
    # (6451, -114.567265, 41.641591, 1e-4, 0.0, 0.0, 1.2961154679e-05),
    # NAD83(2011) - NAD83(2011) / Idaho West
    # (6452, -125.292410, 41.269025, 1e-4, 0.0, 0.0, 0.0284014806996),
    # NAD83(2011) - NAD83(2011) / Idaho West (ftUS)
    # (6453, -125.292410, 41.269025, 1e-4, 0.0, 0.0, 0.0931805209225),
    # NAD83(2011) - NAD83(2011) / Illinois East
    # (6454, -91.686566, 36.619446, 1e-4, 0.0, 0.0, 2.51901485408e-05),
    # NAD83(2011) - NAD83(2011) / Illinois East (ftUS)
    # (6455, -91.686566, 36.619446, 1e-4, 0.0, 0.0, 8.26441060954e-05),
    # NAD83(2011) - NAD83(2011) / Illinois West
    # (6456, -97.964340, 36.410947, 1e-4, 0.0, 0.0, 0.00255660697676),
    # NAD83(2011) - NAD83(2011) / Illinois West (ftUS)
    # (6457, -97.964340, 36.410947, 1e-4, 0.0, 0.0, 0.00838779730119),
    # NAD83(2011) - NAD83(2011) / Indiana East
    # (6458, -86.765357, 35.241995, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Indiana East (ftUS)
    # (6459, -86.765357, 35.241995, 1e-4, 0.0, 0.0, 1.5225054085e-07),
    # NAD83(2011) - NAD83(2011) / Indiana West
    # (6460, -96.907562, 34.847248, 1e-4, 0.0, 0.0, 0.01940148008),
    # NAD83(2011) - NAD83(2011) / Indiana West (ftUS)
    # (6461, -96.907562, 34.847248, 1e-4, 0.0, 0.0, 0.0636530351664),
    # NAD83(2011) - NAD83(2011) / Iowa North
    # (6462, -109.055583, 31.312141, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Iowa North (ftUS)
    # (6463, -109.055583, 31.312141, 1e-4, 0.0, 0.0, 3.24839347741e-07),
    # NAD83(2011) - NAD83(2011) / Iowa South
    # (6464, -99.345436, 39.848662, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Iowa South (ftUS)
    # (6465, -99.345436, 39.848662, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Kansas North
    # (6466, -102.570641, 38.242381, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Kansas North (ftUS)
    # (6467, -102.570641, 38.242381, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Kansas South
    # (6468, -102.765768, 32.984182, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Kansas South (ftUS)
    # (6469, -102.765768, 32.984182, 1e-4, 0.0, 0.0, 2.31455196626e-07),
    # NAD83(2011) - NAD83(2011) / Kentucky North
    # (6470, -89.896811, 37.361875, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Kentucky North (ftUS)
    # (6471, -89.896811, 37.361875, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Kentucky Single Zone
    # (6472, -100.546842, 26.314343, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Kentucky Single Zone (ftUS)
    # (6473, -100.546842, 26.314343, 1e-4, 0.0, 0.0, 4.10775683122e-07),
    # NAD83(2011) - NAD83(2011) / Kentucky South
    # (6474, -91.003100, 31.708902, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Kentucky South (ftUS)
    # (6475, -91.003100, 31.708902, 1e-4, 0.0, 0.0, 2.81011816696e-07),
    # NAD83(2011) - NAD83(2011) / Louisiana North
    # (6476, -102.882630, 30.067691, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Louisiana North (ftUS)
    # (6477, -102.882630, 30.067691, 1e-4, 0.0, 0.0, 3.17932299144e-07),
    # NAD83(2011) - NAD83(2011) / Louisiana South
    # (6478, -101.517796, 28.098894, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Louisiana South (ftUS)
    # (6479, -101.517796, 28.098894, 1e-4, 0.0, 0.0, 3.50488795371e-07),
    # NAD83(2011) - NAD83(2011) / Maine CS2000 Central
    # (6480, -75.289767, 43.333516, 1e-4, 0.0, 0.0, 0.00085338508619),
    # NAD83(2011) - NAD83(2011) / Maine CS2000 East
    # (6481, -76.529297, 43.504477, 1e-4, 0.0, 0.0, 0.0142061173473),
    # NAD83(2011) - NAD83(2011) / Maine CS2000 West
    # (6482, -74.040638, 42.774624, 1e-4, 0.0, 0.0, 4.41511791914e-05),
    # NAD83(2011) - NAD83(2011) / Maine East
    # (6483, -72.216114, 43.606225, 1e-4, 0.0, 0.0, 4.76777649198e-05),
    # NAD83(2011) - NAD83(2011) / Maine East (ftUS)
    # (6484, -72.216114, 43.606225, 1e-4, 0.0, 0.0, 0.000156422227499),
    # NAD83(2011) - NAD83(2011) / Maine West
    # (6485, -81.077225, 42.310713, 1e-4, 0.0, 0.0, 0.102337038947),
    # NAD83(2011) - NAD83(2011) / Maine West (ftUS)
    # (6486, -81.077225, 42.310713, 1e-4, 0.0, 0.0, 0.335750768611),
    # NAD83(2011) - NAD83(2011) / Maryland
    # (6487, -81.529189, 37.577262, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Maryland (ftUS)
    # (6488, -81.529189, 37.577262, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Massachusetts Island
    # (6489, -76.433406, 40.845825, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Massachusetts Island (ftUS)
    # (6490, -76.433406, 40.845825, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Massachusetts Mainland
    # (6491, -73.651391, 34.244387, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Massachusetts Mainland (ftUS)
    # (6492, -73.651391, 34.244387, 1e-4, 0.0, 0.0, 2.03525887628e-07),
    # NAD83(2011) - NAD83(2011) / Michigan Central
    # (6493, -144.322231, 22.874000, 1e-4, 0.0, 0.0, 2.08924502698e-07),
    # NAD83(2011) - NAD83(2011) / Michigan Central (ft)
    # (6494, -144.322231, 22.874000, 1e-4, 0.0, 0.0, 6.80864563111e-07),
    # NAD83(2011) - NAD83(2011) / Michigan North
    # (6495, -158.790109, 11.663980, 1e-4, 0.0, 0.0, 2.29428967657e-07),
    # NAD83(2011) - NAD83(2011) / Michigan North (ft)
    # (6496, -158.790109, 11.663980, 1e-4, 0.0, 0.0, 7.44651609622e-07),
    # NAD83(2011) - NAD83(2011) / Michigan Oblique Mercator
    # (6497, -91.882286, 40.401569, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Michigan South
    # (6498, -127.914179, 32.026364, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Michigan South (ft)
    # (6499, -127.914179, 32.026364, 1e-4, 0.0, 0.0, 3.67591261454e-07),
    # NAD83(2011) - NAD83(2011) / Minnesota Central
    # (6500, -104.182085, 43.648811, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Minnesota Central (ftUS)
    # (6501, -104.182085, 43.648811, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Minnesota North
    # (6502, -103.287887, 45.126033, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Minnesota North (ftUS)
    # (6503, -103.287887, 45.126033, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Minnesota South
    # (6504, -103.618869, 41.676372, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Minnesota South (ftUS)
    # (6505, -103.618869, 41.676372, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Mississippi East
    # (6506, -91.925475, 29.464053, 1e-4, 0.0, 0.0, 1.2232144699e-05),
    # NAD83(2011) - NAD83(2011) / Mississippi East (ftUS)
    # (6507, -91.925475, 29.464053, 1e-4, 0.0, 0.0, 4.01310551576e-05),
    # NAD83(2011) - NAD83(2011) / Mississippi TM
    # (6508, -94.546047, 20.698279, 1e-4, 0.0, 0.0, 5.93323493376e-05),
    # NAD83(2011) - NAD83(2011) / Mississippi West
    # (6509, -97.529032, 29.305235, 1e-4, 0.0, 0.0, 0.00138477058454),
    # NAD83(2011) - NAD83(2011) / Mississippi West (ftUS)
    # (6510, -97.529032, 29.305235, 1e-4, 0.0, 0.0, 0.00454319441734),
    # NAD83(2011) - NAD83(2011) / Missouri Central
    # (6511, -98.022683, 35.706341, 1e-4, 0.0, 0.0, 0.000399243352018),
    # NAD83(2011) - NAD83(2011) / Missouri East
    # (6512, -93.265663, 35.801506, 1e-4, 0.0, 0.0, 8.21736460543e-06),
    # NAD83(2011) - NAD83(2011) / Missouri West
    # (6513, -103.890580, 35.797519, 1e-4, 0.0, 0.0, 0.0140642820106),
    # NAD83(2011) - NAD83(2011) / Montana
    # (6514, -116.985465, 43.991943, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Montana (ft)
    # (6515, -116.985465, 43.991943, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Nebraska
    # (6516, -105.831722, 39.681418, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(2011) / Nebraska (ftUS) (deprecated)
    # (6517, -105.831722, 39.681418, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Nevada Central
    # (6518, -121.423274, -19.408016, 1e-4, 0.0, 0.0, 3.61900019925e-05),
    # NAD83(2011) - NAD83(2011) / Nevada Central (ftUS)
    # (6519, -121.423274, -19.408016, 1e-4, 0.0, 0.0, 0.000118735370052),
    # NAD83(2011) - NAD83(2011) / Nevada East
    # (6520, -117.845072, -37.495818, 1e-4, 0.0, 0.0, 2.70277087111e-06),
    # NAD83(2011) - NAD83(2011) / Nevada East (ftUS)
    # (6521, -117.845072, -37.495818, 1e-4, 0.0, 0.0, 8.85774454218e-06),
    # NAD83(2011) - NAD83(2011) / Nevada West
    # (6522, -125.753742, -1.377653, 1e-4, 0.0, 0.0, 0.00310022570193),
    # NAD83(2011) - NAD83(2011) / Nevada West (ftUS)
    # (6523, -125.753742, -1.377653, 1e-4, 0.0, 0.0, 0.0101713249696),
    # NAD83(2011) - NAD83(2011) / New Hampshire
    # (6524, -75.312874, 42.441965, 1e-4, 0.0, 0.0, 4.28260943867e-05),
    # NAD83(2011) - NAD83(2011) / New Hampshire (ftUS)
    # (6525, -75.312874, 42.441965, 1e-4, 0.0, 0.0, 0.000140504705092),
    # NAD83(2011) - NAD83(2011) / New Jersey
    # (6526, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 6.02422172344e-07),
    # NAD83(2011) - NAD83(2011) / New Jersey (ftUS)
    # (6527, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 1.97644674377e-06),
    # NAD83(2011) - NAD83(2011) / New Mexico Central
    # (6528, -111.476675, 30.894188, 1e-4, 0.0, 0.0, 0.000274516973348),
    # NAD83(2011) - NAD83(2011) / New Mexico Central (ftUS)
    # (6529, -111.476675, 30.894188, 1e-4, 0.0, 0.0, 0.000900648487301),
    # NAD83(2011) - NAD83(2011) / New Mexico East
    # (6530, -106.060830, 30.988445, 1e-4, 0.0, 0.0, 4.79974244135e-07),
    # NAD83(2011) - NAD83(2011) / New Mexico East (ftUS)
    # (6531, -106.060830, 30.988445, 1e-4, 0.0, 0.0, 1.5747154993e-06),
    # NAD83(2011) - NAD83(2011) / New Mexico West
    # (6532, -116.482847, 30.710010, 1e-4, 0.0, 0.0, 0.00241149640912),
    # NAD83(2011) - NAD83(2011) / New Mexico West (ftUS)
    # (6533, -116.482847, 30.710010, 1e-4, 0.0, 0.0, 0.00791171227234),
    # NAD83(2011) - NAD83(2011) / New York Central
    # (6534, -79.509326, 39.963054, 1e-4, 0.0, 0.0, 1.20960787919e-05),
    # NAD83(2011) - NAD83(2011) / New York Central (ftUS)
    # (6535, -79.509326, 39.963054, 1e-4, 0.0, 0.0, 3.96810172177e-05),
    # NAD83(2011) - NAD83(2011) / New York East
    # (6536, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 6.02422172344e-07),
    # NAD83(2011) - NAD83(2011) / New York East (ftUS)
    # (6537, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 1.97644674377e-06),
    # NAD83(2011) - NAD83(2011) / New York Long Island
    # (6538, -77.519584, 40.112385, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / New York Long Island (ftUS)
    # (6539, -77.519584, 40.112385, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / New York West
    # (6540, -82.677307, 39.927648, 1e-4, 0.0, 0.0, 8.10902805652e-05),
    # NAD83(2011) - NAD83(2011) / New York West (ftUS)
    # (6541, -82.677307, 39.927648, 1e-4, 0.0, 0.0, 0.000266040889871),
    # NAD83(2011) - NAD83(2011) / North Carolina
    # (6542, -85.568291, 33.568155, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / North Carolina (ftUS)
    # (6543, -85.568291, 33.568155, 1e-4, 0.0, 0.0, 2.26943686538e-07),
    # NAD83(2011) - NAD83(2011) / North Dakota North
    # (6544, -108.360607, 46.724303, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / North Dakota North (ft)
    # (6545, -108.360607, 46.724303, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / North Dakota South
    # (6546, -108.173906, 45.402819, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / North Dakota South (ft)
    # (6547, -108.173906, 45.402819, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Ohio North
    # (6548, -89.475829, 39.450489, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Ohio North (ftUS)
    # (6549, -89.475829, 39.450489, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Ohio South
    # (6550, -89.316674, 37.795918, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Ohio South (ftUS)
    # (6551, -89.316674, 37.795918, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oklahoma North
    # (6552, -104.561592, 34.817203, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oklahoma North (ftUS)
    # (6553, -104.561592, 34.817203, 1e-4, 0.0, 0.0, 1.91522645799e-07),
    # NAD83(2011) - NAD83(2011) / Oklahoma South
    # (6554, -104.434828, 33.160876, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oklahoma South (ftUS)
    # (6555, -104.434828, 33.160876, 1e-4, 0.0, 0.0, 2.41455848176e-07),
    # NAD83(2011) - NAD83(2011) / Oregon LCC (m)
    # (6556, -125.300329, 41.644767, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon GIC Lambert (ft)
    # (6557, -125.300329, 41.644767, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon North
    # (6558, -150.091081, 39.506127, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon North (ft)
    # (6559, -150.091081, 39.506127, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon South
    # (6560, -138.236892, 40.231913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon South (ft)
    # (6561, -138.236892, 40.231913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Pennsylvania North
    # (6562, -84.776606, 39.947400, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Pennsylvania North (ftUS)
    # (6563, -84.776606, 39.947400, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Pennsylvania South
    # (6564, -84.693691, 39.120795, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Pennsylvania South (ftUS)
    # (6565, -84.693691, 39.120795, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Puerto Rico and Virgin Is.
    # (6566, -68.300715, 16.017453, 1e-4, 0.0, 0.0, 1.52795109898e-07),
    # NAD83(2011) - NAD83(2011) / Rhode Island
    # (6567, -72.689948, 41.077189, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Rhode Island (ftUS)
    # (6568, -72.689948, 41.077189, 1e-4, 0.0, 0.0, 2.51308794467e-07),
    # NAD83(2011) - NAD83(2011) / South Carolina
    # (6569, -87.429394, 31.662328, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / South Carolina (ft)
    # (6570, -87.429394, 31.662328, 1e-4, 0.0, 0.0, 2.6761616217e-07),
    # NAD83(2011) - NAD83(2011) / South Dakota North
    # (6571, -107.437658, 43.585145, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / South Dakota North (ftUS)
    # (6572, -107.437658, 43.585145, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / South Dakota South
    # (6573, -107.593704, 42.097136, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / South Dakota South (ftUS)
    # (6574, -107.593704, 42.097136, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Tennessee
    # (6575, -92.508665, 34.153466, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Tennessee (ftUS)
    # (6576, -92.508665, 34.153466, 1e-4, 0.0, 0.0, 1.98492272166e-07),
    # NAD83(2011) - NAD83(2011) / Texas Central
    # (6577, -105.983204, 3.442337, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Texas Central (ftUS)
    # (6578, -105.983204, 3.442337, 1e-4, 0.0, 0.0, 1.89059937838e-07),
    # NAD83(2011) - NAD83(2011) / Texas Centric Albers Equal Area
    # (6579, -109.212659, -58.634299, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Texas Centric Lambert Conformal
    # (6580, -109.731302, -20.457909, 1e-4, 0.0, 0.0, 2.32248567045e-07),
    # NAD83(2011) - NAD83(2011) / Texas North
    # (6581, -103.450598, 25.015254, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Texas North (ftUS)
    # (6582, -103.450598, 25.015254, 1e-4, 0.0, 0.0, 4.1378345486e-07),
    # NAD83(2011) - NAD83(2011) / Texas North Central
    # (6583, -103.763988, 13.819848, 1e-4, 0.0, 0.0, 1.59605406225e-07),
    # NAD83(2011) - NAD83(2011) / Texas North Central (ftUS)
    # (6584, -103.763988, 13.819848, 1e-4, 0.0, 0.0, 5.39107277291e-07),
    # NAD83(2011) - NAD83(2011) / Texas South
    # (6585, -100.642123, -15.495006, 1e-4, 0.0, 0.0, 2.08325218409e-07),
    # NAD83(2011) - NAD83(2011) / Texas South (ftUS)
    # (6586, -100.642123, -15.495006, 1e-4, 0.0, 0.0, 6.52829694445e-07),
    # NAD83(2011) - NAD83(2011) / Texas South Central
    # (6587, -103.518030, -6.145758, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Texas South Central (ftUS)
    # (6588, -103.518030, -6.145758, 1e-4, 0.0, 0.0, 3.59786790796e-07),
    # NAD83(2011) - NAD83(2011) / Vermont
    # (6589, -78.566435, 42.339184, 1e-4, 0.0, 0.0, 0.000677358422587),
    # NAD83(2011) - NAD83(2011) / Vermont (ftUS)
    # (6590, -78.566435, 42.339184, 1e-4, 0.0, 0.0, 0.00222229043748),
    # NAD83(2011) - NAD83(2011) / Virginia Lambert
    # (6591, -79.500000, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Virginia North
    # (6592, -109.123093, 14.944862, 1e-4, 0.0, 0.0, 2.08849087358e-07),
    # NAD83(2011) - NAD83(2011) / Virginia North (ftUS)
    # (6593, -109.123093, 14.944862, 1e-4, 0.0, 0.0, 6.84053229634e-07),
    # NAD83(2011) - NAD83(2011) / Virginia South
    # (6594, -111.898764, 21.848820, 1e-4, 0.0, 0.0, 1.84925738722e-07),
    # NAD83(2011) - NAD83(2011) / Virginia South (ftUS)
    # (6595, -111.898764, 21.848820, 1e-4, 0.0, 0.0, 6.19314523647e-07),
    # NAD83(2011) - NAD83(2011) / Washington North
    # (6596, -127.390662, 46.808297, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Washington North (ftUS)
    # (6597, -127.390662, 46.808297, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Washington South
    # (6598, -126.863697, 45.151783, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Washington South (ftUS)
    # (6599, -126.863697, 45.151783, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / West Virginia North
    # (6600, -86.363854, 38.293447, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / West Virginia North (ftUS)
    # (6601, -86.363854, 38.293447, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / West Virginia South
    # (6602, -87.727921, 36.803713, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / West Virginia South (ftUS)
    # (6603, -87.727921, 36.803713, 1e-4, 0.0, 0.0, 1.63071231427e-07),
    # NAD83(NSRS2007) - NAD83(2011) / Wisconsin Central (deprecated)
    # (6604, -97.438214, 43.585869, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Wisconsin Central (ftUS)
    # (6605, -97.438214, 43.585869, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Wisconsin North
    # (6606, -97.607760, 44.907938, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Wisconsin North (ftUS)
    # (6607, -97.607760, 44.907938, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Wisconsin South
    # (6608, -97.222171, 41.765988, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Wisconsin South (ftUS)
    # (6609, -97.222171, 41.765988, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Wisconsin Transverse Mercator
    # (6610, -96.117596, 40.308550, 1e-4, 0.0, 0.0, 0.000626261142315),
    # NAD83(2011) - NAD83(2011) / Wyoming East
    # (6611, -107.525253, 40.475927, 1e-4, 0.0, 0.0, 3.54957583129e-06),
    # NAD83(2011) - NAD83(2011) / Wyoming East (ftUS)
    # (6612, -107.525253, 40.475927, 1e-4, 0.0, 0.0, 1.164250832e-05),
    # NAD83(2011) - NAD83(2011) / Wyoming East Central
    # (6613, -111.983494, 39.506205, 1e-4, 0.0, 0.0, 0.000162927870406),
    # NAD83(2011) - NAD83(2011) / Wyoming East Central (ftUS)
    # (6614, -111.983494, 39.506205, 1e-4, 0.0, 0.0, 0.00053454183786),
    # NAD83(2011) - NAD83(2011) / Wyoming West
    # (6615, -119.340930, 39.229368, 1e-4, 0.0, 0.0, 0.0180613152843),
    # NAD83(2011) - NAD83(2011) / Wyoming West (ftUS)
    # (6616, -119.340930, 39.229368, 1e-4, 0.0, 0.0, 0.0592561650137),
    # NAD83(2011) - NAD83(2011) / Wyoming West Central
    # (6617, -115.803136, 40.284354, 1e-4, 0.0, 0.0, 0.00182561155378),
    # NAD83(2011) - NAD83(2011) / Wyoming West Central (ftUS)
    # (6618, -115.803136, 40.284354, 1e-4, 0.0, 0.0, 0.00598952723936),
    # NAD83(2011) - NAD83(2011) / Utah Central
    # (6619, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 1.61089701578e-07),
    # NAD83(2011) - NAD83(2011) / Utah North
    # (6620, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Utah South
    # (6621, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 1.55065208673e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Quebec Lambert
    # (6622, -68.500000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Quebec Albers
    # (6623, -68.500000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CSRS) - NAD83(CSRS) / Quebec Albers
    # (6624, -68.500000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Utah Central (ftUS)
    # (6625, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 5.27076190338e-07),
    # NAD83(2011) - NAD83(2011) / Utah North (ftUS)
    # (6626, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 2.83303452306e-07),
    # NAD83(2011) - NAD83(2011) / Utah South (ftUS)
    # (6627, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 4.83248659293e-07),
    # NAD83(PA11) - NAD83(PA11) / Hawaii zone 1
    # (6628, -160.238151, 18.773126, 1e-4, 0.0, 0.0, 2.68067027011e-05),
    # NAD83(PA11) - NAD83(PA11) / Hawaii zone 2
    # (6629, -161.448688, 20.267936, 1e-4, 0.0, 0.0, 5.14780165235e-05),
    # NAD83(PA11) - NAD83(PA11) / Hawaii zone 3
    # (6630, -162.808055, 21.098347, 1e-4, 0.0, 0.0, 6.66313252259e-05),
    # NAD83(PA11) - NAD83(PA11) / Hawaii zone 4
    # (6631, -164.329911, 21.762651, 1e-4, 0.0, 0.0, 7.90273515641e-05),
    # NAD83(PA11) - NAD83(PA11) / Hawaii zone 5
    # (6632, -164.990986, 21.596578, 1e-4, 0.0, 0.0, 7.59037093138e-05),
    # NAD83(PA11) - NAD83(PA11) / Hawaii zone 3 (ftUS)
    # (6633, -162.808055, 21.098347, 1e-4, 0.0, 0.0, 0.000218613236233),
    # NAD83(PA11) - NAD83(PA11) / UTM zone 4N
    # (6634, -163.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83(PA11) - NAD83(PA11) / UTM zone 5N
    # (6635, -157.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167734193383),
    # NAD83(PA11) - NAD83(PA11) / UTM zone 2S
    # (6636, -171.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # NAD83(MA11) - NAD83(MA11) / Guam Map Grid
    # (6637, 143.832817, 11.690660, 1e-4, 0.0, 0.0, 1e-07),
    # Karbala 1979 - Karbala 1979 / Iraq National Grid
    # (6646, 38.314465, 28.776990, 1e-4, 0.0, 0.0, 0.00232433037019),
    # JGD2011
    # (6668, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS I
    # (6669, 129.500000, 33.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS II
    # (6670, 131.0, 33.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS III
    # (6671, 132.166667, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS IV
    # (6672, 133.500000, 33.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS V
    # (6673, 134.333333, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS VI
    # (6674, 136.0, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS VII
    # (6675, 137.166667, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS VIII
    # (6676, 138.500000, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS IX
    # (6677, 139.833333, 36.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS X
    # (6678, 140.833333, 40.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS XI
    # (6679, 140.250000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS XII
    # (6680, 142.250000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS XIII
    # (6681, 144.250000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS XIV
    # (6682, 142.0, 26.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS XV
    # (6683, 127.500000, 26.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS XVI
    # (6684, 124.0, 26.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS XVII
    # (6685, 131.0, 26.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS XVIII
    # (6686, 136.0, 20.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / Japan Plane Rectangular CS XIX
    # (6687, 154.0, 26.0, 1e-4, 0.0, 0.0, 1e-07),
    # JGD2011 - JGD2011 / UTM zone 51N
    # (6688, 118.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # JGD2011 - JGD2011 / UTM zone 52N
    # (6689, 124.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # JGD2011 - JGD2011 / UTM zone 53N
    # (6690, 130.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # JGD2011 - JGD2011 / UTM zone 54N
    # (6691, 136.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167725724168),
    # JGD2011 - JGD2011 / UTM zone 55N
    # (6692, 142.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # WGS 84 - WGS 84 / TM 60 SW
    # (6703, -60.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RDN2008
    # (6706, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RDN2008 - RDN2008 / TM32
    # (6707, 4.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # RDN2008 - RDN2008 / TM33
    # (6708, 10.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # RDN2008 - RDN2008 / TM34
    # (6709, 16.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # WGS 84 - WGS 84 / CIG92
    # (6720, 105.166301, -11.754510, 1e-4, 0.0, 0.0, 1e-07),
    # GDA94 - GDA94 / CIG94
    # (6721, 105.166301, -11.754496, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / CKIG92
    # (6722, 96.414731, -12.658726, 1e-4, 0.0, 0.0, 1e-07),
    # GDA94 - GDA94 / CKIG94
    # (6723, 96.413045, -13.562682, 1e-4, 0.0, 0.0, 1e-07),
    # GDA94 - GDA94 / MGA zone 41 (deprecated)
    # (6732, 63.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 42 (deprecated)
    # (6733, 69.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 43 (deprecated)
    # (6734, 75.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 44 (deprecated)
    # (6735, 81.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 46
    # (6736, 93.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 47
    # (6737, 99.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 59
    # (6738, 171.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # NAD83(CORS96)
    # (6783, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Baker zone (m)
    # (6784, -118.336201, 44.498893, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Baker zone (ft)
    # (6785, -118.336201, 44.498893, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Baker zone (m)
    # (6786, -118.336201, 44.498893, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Baker zone (ft)
    # (6787, -118.336201, 44.498893, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Bend-Klamath Falls zone (m)
    # (6788, -122.711578, 41.745976, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Bend-Klamath Falls zone (ft)
    # (6789, -122.711578, 41.745976, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Bend-Klamath Falls zone (m)
    # (6790, -122.711578, 41.745976, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Bend-Klamath Falls zone (ft)
    # (6791, -122.711578, 41.745976, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Bend-Redmond-Prineville zone (m)
    # (6792, -122.238739, 43.492550, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Bend-Redmond-Prineville zone (ft)
    # (6793, -122.238739, 43.492550, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Bend-Redmond-Prineville zone (m)
    # (6794, -122.238739, 43.492550, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Bend-Redmond-Prineville zone (ft)
    # (6795, -122.238739, 43.492550, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Bend-Burns zone (m)
    # (6796, -121.224172, 43.117138, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Bend-Burns zone (ft)
    # (6797, -121.224172, 43.117138, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Bend-Burns zone (m)
    # (6798, -121.224172, 43.117138, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Bend-Burns zone (ft)
    # (6799, -121.224172, 43.117138, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Canyonville-Grants Pass zone (m)
    # (6800, -123.819915, 42.498967, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Canyonville-Grants Pass zone (ft)
    # (6801, -123.819915, 42.498967, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Canyonville-Grants Pass zone (m)
    # (6802, -123.819915, 42.498967, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Canyonville-Grants Pass zone (ft)
    # (6803, -123.819915, 42.498967, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Columbia River East zone (m)
    # (6804, -122.415288, 45.380609, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Columbia River East zone (ft)
    # (6805, -122.415288, 45.380609, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Columbia River East zone (m)
    # (6806, -122.415288, 45.380609, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Columbia River East zone (ft)
    # (6807, -122.415288, 45.380609, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Columbia River West zone (m)
    # (6808, -125.105644, 44.226421, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Columbia River West zone (ft)
    # (6809, -125.105644, 44.226421, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Columbia River West zone (m)
    # (6810, -125.105644, 44.226421, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Columbia River West zone (ft)
    # (6811, -125.105644, 44.226421, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Cottage Grove-Canyonville zone (m)
    # (6812, -123.944841, 42.831700, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Cottage Grove-Canyonville zone (ft)
    # (6813, -123.944841, 42.831700, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Cottage Grove-Canyonville zone (m)
    # (6814, -123.944841, 42.831700, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Cottage Grove-Canyonville zone (ft)
    # (6815, -123.944841, 42.831700, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Dufur-Madras zone (m)
    # (6816, -122.005728, 44.495572, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Dufur-Madras zone (ft)
    # (6817, -122.005728, 44.495572, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Dufur-Madras zone (m)
    # (6818, -122.005728, 44.495572, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Dufur-Madras zone (ft)
    # (6819, -122.005728, 44.495572, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Eugene zone (m)
    # (6820, -123.787432, 43.748314, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Eugene zone (ft)
    # (6821, -123.787432, 43.748314, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Eugene zone (m)
    # (6822, -123.787432, 43.748314, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Eugene zone (ft)
    # (6823, -123.787432, 43.748314, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Grants Pass-Ashland zone (m)
    # (6824, -123.934439, 41.748428, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Grants Pass-Ashland zone (ft)
    # (6825, -123.934439, 41.748428, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Grants Pass-Ashland zone (m)
    # (6826, -123.934439, 41.748428, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Grants Pass-Ashland zone (ft)
    # (6827, -123.934439, 41.748428, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Gresham-Warm Springs zone (m)
    # (6828, -122.460155, 44.999930, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Gresham-Warm Springs zone (ft)
    # (6829, -122.460155, 44.999930, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Gresham-Warm Springs zone (m)
    # (6830, -122.460155, 44.999930, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Gresham-Warm Springs zone (ft)
    # (6831, -122.460155, 44.999930, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon La Grande zone (m)
    # (6832, -118.507237, 44.998874, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon La Grande zone (ft)
    # (6833, -118.507237, 44.998874, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon La Grande zone (m)
    # (6834, -118.507237, 44.998874, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon La Grande zone (ft)
    # (6835, -118.507237, 44.998874, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Ontario zone (m)
    # (6836, -117.984935, 43.245760, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Ontario zone (ft)
    # (6837, -117.984935, 43.245760, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Ontario zone (m)
    # (6838, -117.984935, 43.245760, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Ontario zone (ft)
    # (6839, -117.984935, 43.245760, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Coast zone (m)
    # (6840, -125.661505, 41.415804, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Coast zone (ft)
    # (6841, -125.661505, 41.415804, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Coast zone (m)
    # (6842, -125.661505, 41.415804, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Coast zone (ft)
    # (6843, -125.661505, 41.415804, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Pendleton zone (m)
    # (6844, -119.930898, 45.247443, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Pendleton zone (ft)
    # (6845, -119.930898, 45.247443, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Pendleton zone (m)
    # (6846, -119.930898, 45.247443, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Pendleton zone (ft)
    # (6847, -119.930898, 45.247443, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Pendleton-La Grande zone (m)
    # (6848, -118.714300, 45.082698, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Pendleton-La Grande zone (ft)
    # (6849, -118.714300, 45.082698, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Pendleton-La Grande zone (m)
    # (6850, -118.714300, 45.082698, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Pendleton-La Grande zone (ft)
    # (6851, -118.714300, 45.082698, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Portland zone (m)
    # (6852, -124.019242, 45.043002, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Portland zone (ft)
    # (6853, -124.019242, 45.043002, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Portland zone (m)
    # (6854, -124.019242, 45.043002, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Portland zone (ft)
    # (6855, -124.019242, 45.043002, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Salem zone (m)
    # (6856, -123.710222, 44.331613, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Salem zone (ft)
    # (6857, -123.710222, 44.331613, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Salem zone (m)
    # (6858, -123.710222, 44.331613, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Salem zone (ft)
    # (6859, -123.710222, 44.331613, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Santiam Pass zone (m)
    # (6860, -122.500000, 44.083333, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon Santiam Pass zone (ft)
    # (6861, -122.500000, 44.083333, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Santiam Pass zone (m)
    # (6862, -122.500000, 44.083333, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Oregon Santiam Pass (ft)
    # (6863, -122.500000, 44.083333, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon LCC (m)
    # (6867, -125.300329, 41.644767, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon GIC Lambert (ft)
    # (6868, -125.300329, 41.644767, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / Albania TM 2010
    # (6870, 15.513048, 0.0, 1e-4, 0.0, 0.0, 0.000167405640241),
    # RDN2008 - RDN2008 / Italy zone
    # (6875, -40.727342, 0.0, 1e-4, 0.0, 0.0, 127313),
    # RDN2008 - RDN2008 / Zone 12
    # (6876, -14.000938, 0.0, 1e-4, 0.0, 0.0, 88),
    # NAD83(2011) - NAD83(2011) / Wisconsin Central
    # (6879, -97.438214, 43.585869, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Nebraska (ftUS)
    # (6880, -105.831722, 39.681418, 1e-4, 0.0, 0.0, 1e-07),
    # Aden 1925
    # (6881, -0.001824, 0.002424, 1e-4, 0.0, 0.0, 1e-07),
    # Bekaa Valley 1920
    # (6882, -0.000135, 0.002469, 1e-4, 0.0, 0.0, 1e-07),
    # Bioko
    # (6883, -0.000988, 0.003554, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon North
    # (6884, -150.091081, 39.506127, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon North (ft)
    # (6885, -150.091081, 39.506127, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon South
    # (6886, -138.236892, 40.231913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(CORS96) - NAD83(CORS96) / Oregon South (ft)
    # (6887, -138.236892, 40.231913, 1e-4, 0.0, 0.0, 1e-07),
    # South East Island 1943
    # (6892, -0.001615, -0.002421, 1e-4, 0.0, 0.0, 1e-07),
    # Gambia
    # (6894, 0.001581, 0.001673, 1e-4, 0.0, 0.0, 1e-07),
    # South East Island 1943 - South East Island 1943 / UTM zone 40N
    # (6915, 52.510664, -0.002421, 1e-4, 0.0, 0.0, 0.000172127882337),
    # NAD83 - NAD83 / Kansas LCC
    # (6922, -102.679460, 35.913304, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Kansas LCC (ftUS)
    # (6923, -102.679460, 35.913304, 1e-4, 0.0, 0.0, 1.76437575259e-07),
    # NAD83(2011) - NAD83(2011) / Kansas LCC
    # (6924, -102.679460, 35.913304, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / Kansas LCC (ftUS)
    # (6925, -102.679460, 35.913304, 1e-4, 0.0, 0.0, 1.76437575259e-07),
    # WGS 84 - WGS 84 / NSIDC EASE-Grid 2.0 North
    # (6931, 0.0, 90.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / NSIDC EASE-Grid 2.0 South
    # (6932, 0.0, -90.0, 1e-4, 0.0, 0.0, 0.28512494266),
    # WGS 84 - WGS 84 / NSIDC EASE-Grid 2.0 Global
    # (6933, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # VN-2000 - VN-2000 / TM-3 zone 481
    # (6956, 102.001766, -4.523208, 1e-4, 0.0, 0.0, 1e-07),
    # VN-2000 - VN-2000 / TM-3 zone 482
    # (6957, 105.001763, -4.523201, 1e-4, 0.0, 0.0, 1e-07),
    # VN-2000 - VN-2000 / TM-3 zone 491
    # (6958, 108.001755, -4.523194, 1e-4, 0.0, 0.0, 1e-07),
    # VN-2000 - VN-2000 / TM-3 Da Nang zone
    # (6959, 107.751756, -4.523194, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / Albania LCC 2010
    # (6962, 20.0, 41.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGD05
    # (6980, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IG05 Intermediate CRS
    # (6983, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IG05 Intermediate CRS - Israeli Grid 05
    # (6984, 33.011004, 26.061525, 1e-4, 0.0, 0.0, 1.21995981317e-06),
    # IGD05/12
    # (6987, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IG05/12 Intermediate CRS
    # (6990, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IG05/12 Intermediate CRS - Israeli Grid 05/12
    # (6991, 33.011004, 26.061525, 1e-4, 0.0, 0.0, 1.21995981317e-06),
    # NAD83(2011) - NAD83(2011) / San Francisco CS13 (deprecated)
    # (6996, -122.993060, 37.532517, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / San Francisco CS13 (ftUS) (deprecated)
    # (6997, -122.993060, 37.532517, 1e-4, 0.0, 0.0, 1e-07),
    # Nahrwan 1934 - Nahrwan 1934 / UTM zone 37N
    # (7005, 34.511495, 0.003349, 1e-4, 0.0, 0.0, 0.000172126834599),
    # Nahrwan 1934 - Nahrwan 1934 / UTM zone 38N
    # (7006, 40.511759, 0.003349, 1e-4, 0.0, 0.0, 0.000172129017386),
    # Nahrwan 1934 - Nahrwan 1934 / UTM zone 39N
    # (7007, 46.512018, 0.003349, 1e-4, 0.0, 0.0, 0.000172127882252),
    # RGSPM06 (lon-lat)
    # (7035, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGR92 (lon-lat)
    # (7037, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGM04 (lon-lat)
    # (7039, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGFG95 (lon-lat)
    # (7041, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 1
    # (7057, -124.193268, 12.658218, 1e-4, 0.0, 0.0, 6.76032504998e-07),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 2
    # (7058, -123.810089, 11.332182, 1e-4, 0.0, 0.0, 6.71449233778e-07),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 3
    # (7059, -127.070177, 14.250952, 1e-4, 0.0, 0.0, 8653),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 4
    # (7060, -131.452207, 11.571516, 1e-4, 0.0, 0.0, 6.66865962557e-07),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 5
    # (7061, -130.759361, 9.988172, 1e-4, 0.0, 0.0, 6.32491428405e-07),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 6
    # (7062, -138.863195, 16.485269, 1e-4, 0.0, 0.0, 90511),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 7
    # (7063, -139.624401, 15.589669, 1e-4, 0.0, 0.0, 149800),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 8
    # (7064, -140.469957, 14.733651, 1e-4, 0.0, 0.0, 240858),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 9
    # (7065, -141.226575, 13.925614, 1e-4, 0.0, 0.0, 377189),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 10
    # (7066, -141.338095, 5.418895, 1e-4, 0.0, 0.0, 4.12494409829e-07),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 11
    # (7067, -141.908046, 12.490843, 1e-4, 0.0, 0.0, 862000),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 12
    # (7068, -149.517610, 5.595557, 1e-4, 0.0, 0.0, 4.41139854956e-07),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 13
    # (7069, -145.516447, 14.302699, 1e-4, 0.0, 0.0, 2732530),
    # NAD83(2011) - NAD83(2011) / IaRCS zone 14
    # (7070, -145.260765, 14.776661, 1e-4, 0.0, 0.0, 4235990),
    # RGTAAF07
    # (7073, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGTAAF07 - RGTAAF07 / UTM zone 37S
    # (7074, 39.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGTAAF07 - RGTAAF07 / UTM zone 38S
    # (7075, 45.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGTAAF07 - RGTAAF07 / UTM zone 39S
    # (7076, 51.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGTAAF07 - RGTAAF07 / UTM zone 40S
    # (7077, 57.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGTAAF07 - RGTAAF07 / UTM zone 41S
    # (7078, 63.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGTAAF07 - RGTAAF07 / UTM zone 42S
    # (7079, 69.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGTAAF07 - RGTAAF07 / UTM zone 43S
    # (7080, 75.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGTAAF07 - RGTAAF07 / UTM zone 44S
    # (7081, 81.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # RGF93 (lon-lat)
    # (7084, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGAF09 (lon-lat)
    # (7086, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # RGTAAF07 (lon-lat)
    # (7088, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS St Mary (m)
    # (7109, -114.528744, 48.482118, 1e-4, 0.0, 0.0, 1.46125363646e-06),
    # NAD83(2011) - NAD83(2011) / RMTCRS Blackfeet (m)
    # (7110, -113.839583, 47.992189, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS Milk River (m)
    # (7111, -112.959677, 46.684436, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS Fort Belknap (m)
    # (7112, -111.135009, 47.120233, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS Fort Peck Assiniboine (m)
    # (7113, -108.149495, 47.403034, 1e-4, 0.0, 0.0, 2.97417864203e-06),
    # NAD83(2011) - NAD83(2011) / RMTCRS Fort Peck Sioux (m)
    # (7114, -106.836728, 47.875871, 1e-4, 0.0, 0.0, 2.71767930826e-06),
    # NAD83(2011) - NAD83(2011) / RMTCRS Crow (m)
    # (7115, -110.274008, 44.722101, 1e-4, 0.0, 0.0, 5.27458262385e-06),
    # NAD83(2011) - NAD83(2011) / RMTCRS Bobcat (m)
    # (7116, -112.525584, 45.343264, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS Billings (m)
    # (7117, -110.966746, 45.304835, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS Wind River (m)
    # (7118, -109.552710, 42.660176, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS St Mary (ft)
    # (7119, -114.528744, 48.482118, 1e-4, 0.0, 0.0, 4.78785342421e-06),
    # NAD83(2011) - NAD83(2011) / RMTCRS Blackfeet (ft)
    # (7120, -113.839583, 47.992189, 1e-4, 0.0, 0.0, 4.50149438197e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS Milk River (ft)
    # (7121, -112.959677, 46.684436, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS Fort Belknap (ft)
    # (7122, -111.135009, 47.120233, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS Fort Peck Assiniboine (ft)
    # (7123, -108.149495, 47.403034, 1e-4, 0.0, 0.0, 9.752719367e-06),
    # NAD83(2011) - NAD83(2011) / RMTCRS Fort Peck Sioux (ft)
    # (7124, -106.836728, 47.875871, 1e-4, 0.0, 0.0, 8.9097538446e-06),
    # NAD83(2011) - NAD83(2011) / RMTCRS Crow (ft)
    # (7125, -110.274008, 44.722101, 1e-4, 0.0, 0.0, 1.72996726405e-05),
    # NAD83(2011) - NAD83(2011) / RMTCRS Bobcat (ft)
    # (7126, -112.525584, 45.343264, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS Billings (ft)
    # (7127, -110.966746, 45.304835, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / RMTCRS Wind River (ftUS)
    # (7128, -109.552710, 42.660176, 1e-4, 0.0, 0.0, 2.87079146901e-07),
    # NAD83(2011) - NAD83(2011) / San Francisco CS13
    # (7131, -122.993060, 37.532517, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / San Francisco CS13 (ftUS)
    # (7132, -122.993060, 37.532517, 1e-4, 0.0, 0.0, 1e-07),
    # RGTAAF07 (lon-lat)
    # (7133, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGD05
    # (7136, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # IGD05/12
    # (7139, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # Palestine 1923 - Palestine 1923 / Palestine Grid modified
    # (7142, 33.438014, 30.578183, 1e-4, 0.0, 0.0, 0.026458426677),
    # NAD83(2011) - NAD83(2011) / InGCS Adams (m)
    # (7257, -87.768107, 40.191493, 1e-4, 0.0, 0.0, 9.77875606623e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Adams (ftUS)
    # (7258, -87.768107, 40.191493, 1e-4, 0.0, 0.0, 3.20824688606e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Allen (m)
    # (7259, -87.882725, 40.541087, 1e-4, 0.0, 0.0, 1.01003024611e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Allen (ftUS)
    # (7260, -87.882725, 40.541087, 1e-4, 0.0, 0.0, 3.3137408991e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Bartholomew (m)
    # (7261, -88.606420, 38.643232, 1e-4, 0.0, 0.0, 8.47816409077e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Bartholomew (ftUS)
    # (7262, -88.606420, 38.643232, 1e-4, 0.0, 0.0, 2.78154433545e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Benton (m)
    # (7263, -90.113994, 40.091605, 1e-4, 0.0, 0.0, 9.68814856606e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Benton (ftUS)
    # (7264, -90.113994, 40.091605, 1e-4, 0.0, 0.0, 3.17852007538e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Blackford-Delaware (m)
    # (7265, -88.197665, 39.692065, 1e-4, 0.0, 0.0, 9.3365124485e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Blackford-Delaware (ftUS)
    # (7266, -88.197665, 39.692065, 1e-4, 0.0, 0.0, 3.06315412581e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Boone-Hendricks (m)
    # (7267, -89.279715, 39.242570, 1e-4, 0.0, 0.0, 8.95855191629e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Boone-Hendricks (ftUS)
    # (7268, -89.279715, 39.242570, 1e-4, 0.0, 0.0, 2.93915157454e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Brown (m)
    # (7269, -89.056409, 38.643233, 1e-4, 0.0, 0.0, 8.47633054946e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Brown (ftUS)
    # (7270, -89.056409, 38.643233, 1e-4, 0.0, 0.0, 2.7809427811e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Carroll (m)
    # (7271, -89.461947, 40.041662, 1e-4, 0.0, 0.0, 9.64574792306e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Carroll (ftUS)
    # (7272, -89.461947, 40.041662, 1e-4, 0.0, 0.0, 3.16460913109e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Cass (m)
    # (7273, -89.218124, 40.191490, 1e-4, 0.0, 0.0, 9.77758827503e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Cass (ftUS)
    # (7274, -89.218124, 40.191490, 1e-4, 0.0, 0.0, 3.20786375323e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Clark-Floyd-Scott (m)
    # (7275, -88.324561, 37.794152, 1e-4, 0.0, 0.0, 7.83375071478e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Clark-Floyd-Scott (ftUS)
    # (7276, -88.324561, 37.794152, 1e-4, 0.0, 0.0, 2.57012304701e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Clay (m)
    # (7277, -89.912191, 38.793066, 1e-4, 0.0, 0.0, 8.59365536598e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Clay (ftUS)
    # (7278, -89.912191, 38.793066, 1e-4, 0.0, 0.0, 2.81943509799e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Clinton (m)
    # (7279, -89.401727, 39.791949, 1e-4, 0.0, 0.0, 9.42515544011e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Clinton (ftUS)
    # (7280, -89.401727, 39.791949, 1e-4, 0.0, 0.0, 3.09223641398e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Crawford-Lawrence-Orange (m)
    # (7281, -89.222717, 37.744208, 1e-4, 0.0, 0.0, 7.79844413046e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Crawford-Lawrence-Orange (ftUS)
    # (7282, -89.222717, 37.744208, 1e-4, 0.0, 0.0, 2.55853954513e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Daviess-Greene (m)
    # (7283, -89.835665, 38.093828, 1e-4, 0.0, 0.0, 8.05617673905e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Daviess-Greene (ftUS)
    # (7284, -89.835665, 38.093828, 1e-4, 0.0, 0.0, 2.64309731847e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Dearborn-Ohio-Switzerland (m)
    # (7285, -87.643124, 38.293615, 1e-4, 0.0, 0.0, 8.20605782792e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Dearborn-Ohio-Switzerland (ftUS)
    # (7286, -87.643124, 38.293615, 1e-4, 0.0, 0.0, 2.69227080571e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Decatur-Rush (m)
    # (7287, -88.410231, 38.743125, 1e-4, 0.0, 0.0, 8.5560241132e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Decatur-Rush (ftUS)
    # (7288, -88.410231, 38.743125, 1e-4, 0.0, 0.0, 2.80708891114e-05),
    # NAD83(2011) - NAD83(2011) / InGCS DeKalb (m)
    # (7289, -87.797580, 40.890680, 1e-4, 0.0, 0.0, 1.04330902104e-05),
    # NAD83(2011) - NAD83(2011) / InGCS DeKalb (ftUS)
    # (7290, -87.797580, 40.890680, 1e-4, 0.0, 0.0, 3.4229230132e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Dubois-Martin (m)
    # (7291, -89.676401, 37.844098, 1e-4, 0.0, 0.0, 7.87222234067e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Dubois-Martin (ftUS)
    # (7292, -89.676401, 37.844098, 1e-4, 0.0, 0.0, 2.58274494627e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Elkhart-Kosciusko-Wabash (m)
    # (7293, -88.672258, 40.291377, 1e-4, 0.0, 0.0, 9.87101157079e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Elkhart-Kosciusko-Wabash (ftUS)
    # (7294, -88.672258, 40.291377, 1e-4, 0.0, 0.0, 3.23851437952e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Fayette-Franklin-Union (m)
    # (7295, -87.816020, 38.892961, 1e-4, 0.0, 0.0, 8.67328344611e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Fayette-Franklin-Union (ftUS)
    # (7296, -87.816020, 38.892961, 1e-4, 0.0, 0.0, 2.84555974395e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Fountain-Warren (m)
    # (7297, -90.093676, 39.592173, 1e-4, 0.0, 0.0, 9.25297717913e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Fountain-Warren (ftUS)
    # (7298, -90.093676, 39.592173, 1e-4, 0.0, 0.0, 3.03574759619e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Fulton-Marshall-St. Joseph (m)
    # (7299, -89.132725, 40.541087, 1e-4, 0.0, 0.0, 1.009877451e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Fulton-Marshall-St. Joseph (ftUS)
    # (7300, -89.132725, 40.541087, 1e-4, 0.0, 0.0, 3.31323960381e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Gibson (m)
    # (7301, -90.374582, 37.794149, 1e-4, 0.0, 0.0, 7.83634823165e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Gibson (ftUS)
    # (7302, -90.374582, 37.794149, 1e-4, 0.0, 0.0, 2.570975249e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Grant (m)
    # (7303, -88.509874, 39.991722, 1e-4, 0.0, 0.0, 9.60144825513e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Grant (ftUS)
    # (7304, -88.509874, 39.991722, 1e-4, 0.0, 0.0, 3.15007514837e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Hamilton-Tipton (m)
    # (7305, -88.791646, 39.542233, 1e-4, 0.0, 0.0, 9.20839374885e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Hamilton-Tipton (ftUS)
    # (7306, -88.791646, 39.542233, 1e-4, 0.0, 0.0, 3.02112051577e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Hancock-Madison (m)
    # (7307, -88.581690, 39.292515, 1e-4, 0.0, 0.0, 8.99833321455e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Hancock-Madison (ftUS)
    # (7308, -88.581690, 39.292515, 1e-4, 0.0, 0.0, 2.95220315547e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Harrison-Washington (m)
    # (7309, -88.867238, 37.594369, 1e-4, 0.0, 0.0, 7.69052712712e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Harrison-Washington (ftUS)
    # (7310, -88.867238, 37.594369, 1e-4, 0.0, 0.0, 2.52313377496e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Henry (m)
    # (7311, -88.235636, 39.392405, 1e-4, 0.0, 0.0, 9.0832108981e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Henry (ftUS)
    # (7312, -88.235636, 39.392405, 1e-4, 0.0, 0.0, 2.98005010882e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Howard-Miami (m)
    # (7313, -88.959882, 39.991721, 1e-4, 0.0, 0.0, 9.59802127909e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Howard-Miami (ftUS)
    # (7314, -88.959882, 39.991721, 1e-4, 0.0, 0.0, 3.14895081465e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Huntington-Whitley (m)
    # (7315, -88.322255, 40.291377, 1e-4, 0.0, 0.0, 9.87047678791e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Huntington-Whitley (ftUS)
    # (7316, -88.322255, 40.291377, 1e-4, 0.0, 0.0, 3.23833892617e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Jackson (m)
    # (7317, -88.695027, 38.343558, 1e-4, 0.0, 0.0, 8.24224844109e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Jackson (ftUS)
    # (7318, -88.695027, 38.343558, 1e-4, 0.0, 0.0, 2.70414434272e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Jasper-Porter (m)
    # (7319, -89.924357, 40.341317, 1e-4, 0.0, 0.0, 9.91494016489e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Jasper-Porter (ftUS)
    # (7320, -89.924357, 40.341317, 1e-4, 0.0, 0.0, 3.2529266191e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Jay (m)
    # (7321, -87.807817, 39.941781, 1e-4, 0.0, 0.0, 9.55602445174e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Jay (ftUS)
    # (7322, -87.807817, 39.941781, 1e-4, 0.0, 0.0, 3.13517235554e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Jefferson (m)
    # (7323, -88.089373, 38.193724, 1e-4, 0.0, 0.0, 8.12957296148e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Jefferson (ftUS)
    # (7324, -88.089373, 38.193724, 1e-4, 0.0, 0.0, 2.66717739578e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Jennings (m)
    # (7325, -88.548801, 38.443450, 1e-4, 0.0, 0.0, 8.31999932416e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Jennings (ftUS)
    # (7326, -88.548801, 38.443450, 1e-4, 0.0, 0.0, 2.7296531116e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Johnson-Marion (m)
    # (7327, -88.917980, 38.942902, 1e-4, 0.0, 0.0, 8.71385054779e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Johnson-Marion (ftUS)
    # (7328, -88.917980, 38.942902, 1e-4, 0.0, 0.0, 2.85886913389e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Knox (m)
    # (7329, -90.183812, 38.043881, 1e-4, 0.0, 0.0, 8.0187965068e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Knox (ftUS)
    # (7330, -90.183812, 38.043881, 1e-4, 0.0, 0.0, 2.63083348727e-05),
    # NAD83(2011) - NAD83(2011) / InGCS LaGrange-Noble (m)
    # (7331, -88.297577, 40.890680, 1e-4, 0.0, 0.0, 1.04320097307e-05),
    # NAD83(2011) - NAD83(2011) / InGCS LaGrange-Noble (ftUS)
    # (7332, -88.297577, 40.890680, 1e-4, 0.0, 0.0, 3.42256852582e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Lake-Newton (m)
    # (7333, -90.224360, 40.341317, 1e-4, 0.0, 0.0, 9.91642446024e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Lake-Newton (ftUS)
    # (7334, -90.224360, 40.341317, 1e-4, 0.0, 0.0, 3.25341359166e-05),
    # NAD83(2011) - NAD83(2011) / InGCS LaPorte-Pulaski-Starke (m)
    # (7335, -89.582736, 40.541085, 1e-4, 0.0, 0.0, 1.01005643955e-05),
    # NAD83(2011) - NAD83(2011) / InGCS LaPorte-Pulaski-Starke (ftUS)
    # (7336, -89.582736, 40.541085, 1e-4, 0.0, 0.0, 3.31382683544e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Monroe-Morgan (m)
    # (7337, -89.254502, 38.593287, 1e-4, 0.0, 0.0, 8.43578527565e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Monroe-Morgan (ftUS)
    # (7338, -89.254502, 38.593287, 1e-4, 0.0, 0.0, 2.76764055252e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Montgomery-Putnam (m)
    # (7339, -89.723833, 39.092736, 1e-4, 0.0, 0.0, 8.83572647581e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Montgomery-Putnam (ftUS)
    # (7340, -89.723833, 39.092736, 1e-4, 0.0, 0.0, 2.8988545946e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Owen (m)
    # (7341, -89.662185, 38.793066, 1e-4, 0.0, 0.0, 8.5940482677e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Owen (ftUS)
    # (7342, -89.662185, 38.793066, 1e-4, 0.0, 0.0, 2.81956400249e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Parke-Vermillion (m)
    # (7343, -90.129754, 39.242565, 1e-4, 0.0, 0.0, 8.95935954759e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Parke-Vermillion (ftUS)
    # (7344, -90.129754, 39.242565, 1e-4, 0.0, 0.0, 2.9394165449e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Perry (m)
    # (7345, -89.411825, 37.444526, 1e-4, 0.0, 0.0, 7.58871101425e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Perry (ftUS)
    # (7346, -89.411825, 37.444526, 1e-4, 0.0, 0.0, 2.48972960526e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Pike-Warrick (m)
    # (7347, -90.013645, 37.494471, 1e-4, 0.0, 0.0, 7.62337367632e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Pike-Warrick (ftUS)
    # (7348, -90.013645, 37.494471, 1e-4, 0.0, 0.0, 2.50110184697e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Posey (m)
    # (7349, -90.660042, 37.394576, 1e-4, 0.0, 0.0, 7.5503703556e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Posey (ftUS)
    # (7350, -90.660042, 37.394576, 1e-4, 0.0, 0.0, 2.47715067417e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Randolph-Wayne (m)
    # (7351, -87.833648, 39.342462, 1e-4, 0.0, 0.0, 9.04010084923e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Randolph-Wayne (ftUS)
    # (7352, -87.833648, 39.342462, 1e-4, 0.0, 0.0, 2.96590642029e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Ripley (m)
    # (7353, -88.052567, 38.543346, 1e-4, 0.0, 0.0, 8.39835047373e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Ripley (ftUS)
    # (7354, -88.052567, 38.543346, 1e-4, 0.0, 0.0, 2.75535881792e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Shelby (m)
    # (7355, -88.667983, 38.942902, 1e-4, 0.0, 0.0, 8.7132830231e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Shelby (ftUS)
    # (7356, -88.667983, 38.942902, 1e-4, 0.0, 0.0, 2.85868293849e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Spencer (m)
    # (7357, -89.760039, 37.394576, 1e-4, 0.0, 0.0, 7.5508505688e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Spencer (ftUS)
    # (7358, -89.760039, 37.394576, 1e-4, 0.0, 0.0, 2.47730822412e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Steuben (m)
    # (7359, -87.858348, 41.140387, 1e-4, 0.0, 0.0, 1.0677944374e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Steuben (ftUS)
    # (7360, -87.858348, 41.140387, 1e-4, 0.0, 0.0, 3.50325558338e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Sullivan (m)
    # (7361, -90.252624, 38.543338, 1e-4, 0.0, 0.0, 8.40031498228e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Sullivan (ftUS)
    # (7362, -90.252624, 38.543338, 1e-4, 0.0, 0.0, 2.75600334044e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Tippecanoe-White (m)
    # (7363, -89.703774, 39.841890, 1e-4, 0.0, 0.0, 9.46636646404e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Tippecanoe-White (ftUS)
    # (7364, -89.703774, 39.841890, 1e-4, 0.0, 0.0, 3.10575706408e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Vanderburgh (m)
    # (7365, -90.261839, 37.444524, 1e-4, 0.0, 0.0, 7.58679016144e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Vanderburgh (ftUS)
    # (7366, -90.261839, 37.444524, 1e-4, 0.0, 0.0, 2.48909940547e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Vigo (m)
    # (7367, -90.216069, 38.892954, 1e-4, 0.0, 0.0, 8.67432027007e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Vigo (ftUS)
    # (7368, -90.216069, 38.892954, 1e-4, 0.0, 0.0, 2.84589990861e-05),
    # NAD83(2011) - NAD83(2011) / InGCS Wells (m)
    # (7369, -88.068107, 40.191493, 1e-4, 0.0, 0.0, 9.77875606623e-06),
    # NAD83(2011) - NAD83(2011) / InGCS Wells (ftUS)
    # (7370, -88.068107, 40.191493, 1e-4, 0.0, 0.0, 3.20824688606e-05),
    # ONGD14
    # (7373, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # ONGD14 - ONGD14 / UTM zone 39N
    # (7374, 46.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ONGD14 - ONGD14 / UTM zone 40N
    # (7375, 52.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ONGD14 - ONGD14 / UTM zone 41N
    # (7376, 58.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83(2011) - NAD83(2011) / WISCRS Adams and Juneau (m)
    # (7528, -91.815780, 43.352250, 1e-4, 0.0, 0.0, 8.07514370442e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Ashland (m)
    # (7529, -92.840570, 45.684569, 1e-4, 0.0, 0.0, 2.50741173997e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Barron (m)
    # (7530, -93.033970, 45.127196, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Bayfield (m)
    # (7531, -94.066823, 45.295292, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Brown (m)
    # (7532, -88.387263, 42.957939, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Buffalo (m)
    # (7533, -93.962720, 43.460879, 1e-4, 0.0, 0.0, 2.20087517982e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Burnett (m)
    # (7534, -93.274674, 45.360941, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Calumet, Outagamie and Winnebago (m)
    # (7535, -91.485996, 42.680507, 1e-4, 0.0, 0.0, 1.37943770121e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Chippewa (m)
    # (7536, -92.050435, 44.578591, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Clark (m)
    # (7537, -93.183444, 43.573199, 1e-4, 0.0, 0.0, 4.7227233779e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Columbia (m)
    # (7538, -91.450262, 42.439542, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Crawford (m)
    # (7539, -92.326580, 42.708183, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Dane (m)
    # (7540, -92.392080, 41.710619, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Dodge and Jefferson (m)
    # (7541, -91.925265, 41.429067, 1e-4, 0.0, 0.0, 1.86300136689e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Door (m)
    # (7542, -89.264937, 44.382614, 1e-4, 0.0, 0.0, 1.36708994773e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Douglas (m)
    # (7543, -92.678350, 45.880795, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Dunn (m)
    # (7544, -92.544911, 44.406481, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Eau Claire (m)
    # (7545, -92.786985, 44.037254, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Florence (m)
    # (7546, -89.847458, 45.426150, 1e-4, 0.0, 0.0, 5.59693773605e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Forest (m)
    # (7547, -92.069555, 43.953857, 1e-4, 0.0, 0.0, 3.06875635134e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Grant (m)
    # (7548, -93.696289, 41.374646, 1e-4, 0.0, 0.0, 1.15310710132e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Green and Lafayette (m)
    # (7549, -91.898454, 42.206359, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Green Lake and Marquette (m)
    # (7550, -91.094305, 43.079246, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Iowa (m)
    # (7551, -91.537415, 42.530623, 1e-4, 0.0, 0.0, 1.71625669556e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Iron (m)
    # (7552, -93.077663, 45.398457, 1e-4, 0.0, 0.0, 9.97635576509e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Jackson (m)
    # (7553, -91.181075, 44.027855, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Kenosha, Milwaukee, Racine (m)
    # (7554, -90.145335, 42.194578, 1e-4, 0.0, 0.0, 2.73990274807e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Kewaunee, Manitowoc and Sheboygan (m)
    # (7555, -88.533528, 43.262438, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS La Crosse (m)
    # (7556, -92.928009, 43.439757, 1e-4, 0.0, 0.0, 4.12894875747e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Langlade (m)
    # (7557, -91.514645, 44.179532, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Lincoln (m)
    # (7558, -91.201878, 44.835002, 1e-4, 0.0, 0.0, 2.38555623828e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Marathon (m)
    # (7559, -90.707335, 44.401676, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Marinette (m)
    # (7560, -90.719733, 44.652020, 1e-4, 0.0, 0.0, 1.44110776711e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Menominee (m)
    # (7561, -89.747439, 44.708913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Monroe (m)
    # (7562, -93.144592, 42.874848, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Oconto (m)
    # (7563, -90.202862, 44.374170, 1e-4, 0.0, 0.0, 3.05952905738e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Oneida (m)
    # (7564, -90.436321, 45.182597, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Pepin and Pierce (m)
    # (7565, -94.312347, 43.842662, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Polk (m)
    # (7566, -94.419884, 44.647136, 1e-4, 0.0, 0.0, 7.3098176264e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Portage (m)
    # (7567, -90.202579, 43.964489, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Price (m)
    # (7568, -93.356383, 44.519545, 1e-4, 0.0, 0.0, 1.09545358423e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Richland (m)
    # (7569, -92.876391, 42.087211, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Rock (m)
    # (7570, -90.836103, 41.930894, 1e-4, 0.0, 0.0, 6.88299299698e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Rusk (m)
    # (7571, -94.183787, 43.876910, 1e-4, 0.0, 0.0, 1.76482617609e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Sauk (m)
    # (7572, -92.168857, 42.796962, 1e-4, 0.0, 0.0, 2.87086111869e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Sawyer (m)
    # (7573, -93.854626, 44.780433, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Shawano (m)
    # (7574, -91.876779, 43.989259, 1e-4, 0.0, 0.0, 2.32199437288e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS St. Croix (m)
    # (7575, -94.697360, 44.017464, 1e-4, 0.0, 0.0, 1.67161552222e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Taylor (m)
    # (7576, -92.823746, 44.183937, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Trempealeau (m)
    # (7577, -94.523609, 43.117539, 1e-4, 0.0, 0.0, 1.89645571914e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Vernon (m)
    # (7578, -93.517443, 43.114282, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Vilas (m)
    # (7579, -91.212069, 45.611901, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Walworth (m)
    # (7580, -91.332731, 41.634881, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Washburn (m)
    # (7581, -94.712336, 44.227972, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Washington (m)
    # (7582, -89.534418, 42.908610, 1e-4, 0.0, 0.0, 2.46883901259e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Waukesha (m)
    # (7583, -90.766546, 42.541250, 1e-4, 0.0, 0.0, 5.48915192912e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Waupaca (m)
    # (7584, -91.100301, 43.397471, 1e-4, 0.0, 0.0, 2.98239069683e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Waushara (m)
    # (7585, -90.731413, 43.698552, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Wood (m)
    # (7586, -92.561588, 43.122063, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Adams and Juneau (ftUS)
    # (7587, -91.815780, 43.352250, 1e-4, 0.0, 0.0, 2.65281454762e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Ashland (ftUS)
    # (7588, -92.840570, 45.684569, 1e-4, 0.0, 0.0, 8.21861330397e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Barron (ftUS)
    # (7589, -93.033970, 45.127196, 1e-4, 0.0, 0.0, 2.3747982361e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Bayfield (ftUS)
    # (7590, -94.066823, 45.295292, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Brown (ftUS)
    # (7591, -88.387263, 42.957939, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Buffalo (ftUS)
    # (7592, -93.962720, 43.460879, 1e-4, 0.0, 0.0, 7.2258994837e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Burnett (ftUS)
    # (7593, -93.274674, 45.360941, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Calumet, Fond du Lac, Winnebago (ftUS)
    # (7594, -91.485996, 42.680507, 1e-4, 0.0, 0.0, 4.52610413519e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Chippewa (ftUS)
    # (7595, -92.050435, 44.578591, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Clark (ftUS)
    # (7596, -93.183444, 43.573199, 1e-4, 0.0, 0.0, 1.54953689506e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Columbia (ftUS)
    # (7597, -91.450262, 42.439542, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Crawford (ftUS)
    # (7598, -92.326580, 42.708183, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Dane (ftUS)
    # (7599, -92.392080, 41.710619, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Dodge and Jefferson (ftUS)
    # (7600, -91.925265, 41.429067, 1e-4, 0.0, 0.0, 6.1120245381e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Door (ftUS)
    # (7601, -89.264937, 44.382614, 1e-4, 0.0, 0.0, 4.48469764481e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Douglas (ftUS)
    # (7602, -92.678350, 45.880795, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Dunn (ftUS)
    # (7603, -92.544911, 44.406481, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Eau Claire (ftUS)
    # (7604, -92.786985, 44.037254, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Florence (ftUS)
    # (7605, -89.847458, 45.426150, 1e-4, 0.0, 0.0, 1.834373786e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Forest (ftUS)
    # (7606, -92.069555, 43.953857, 1e-4, 0.0, 0.0, 0.000100677903169),
    # NAD83(2011) - NAD83(2011) / WISCRS Grant (ftUS)
    # (7607, -93.696289, 41.374646, 1e-4, 0.0, 0.0, 3.78166216933e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Green and Lafayette (ftUS)
    # (7608, -91.898454, 42.206359, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Green Lake and Marquette (ftUS)
    # (7609, -91.094305, 43.079246, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Iowa (ftUS)
    # (7610, -91.537415, 42.530623, 1e-4, 0.0, 0.0, 5.54111751215e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Iron (ftUS)
    # (7611, -93.077663, 45.398457, 1e-4, 0.0, 0.0, 3.27335204432e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Jackson (ftUS)
    # (7612, -91.181075, 44.027855, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Kenosha, Milwaukee, and Racine (ftUS)
    # (7613, -90.145335, 42.194578, 1e-4, 0.0, 0.0, 8.99072986074e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Kewaunee, and Sheboygan (ftUS)
    # (7614, -88.533528, 43.262438, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS La Crosse (ftUS)
    # (7615, -92.928009, 43.439757, 1e-4, 0.0, 0.0, 1.34905077254e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Langlade (ftUS)
    # (7616, -91.514645, 44.179532, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Lincoln (ftUS)
    # (7617, -91.201878, 44.835002, 1e-4, 0.0, 0.0, 7.92217514138e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Marathon (ftUS)
    # (7618, -90.707335, 44.401676, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Marinette (ftUS)
    # (7619, -90.719733, 44.652020, 1e-4, 0.0, 0.0, 4.72802919776e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Menominee (ftUS)
    # (7620, -89.747439, 44.708913, 1e-4, 0.0, 0.0, 4.546989564e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Monroe (ftUS)
    # (7621, -93.144592, 42.874848, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Oconto (ftUS)
    # (7622, -90.202862, 44.374170, 1e-4, 0.0, 0.0, 1.00351951946e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Oneida (ftUS)
    # (7623, -90.436321, 45.182597, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Pepin and Pierce (ftUS)
    # (7624, -94.312347, 43.842662, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Polk (ftUS)
    # (7625, -94.419884, 44.647136, 1e-4, 0.0, 0.0, 2.40868419741e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Portage (ftUS)
    # (7626, -90.202579, 43.964489, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Price (ftUS)
    # (7627, -93.356383, 44.519545, 1e-4, 0.0, 0.0, 3.59430505823e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Richland (ftUS)
    # (7628, -92.876391, 42.087211, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Rock (ftUS)
    # (7629, -90.836103, 41.930894, 1e-4, 0.0, 0.0, 2.24821026323e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Rusk (ftUS)
    # (7630, -94.183787, 43.876910, 1e-4, 0.0, 0.0, 5.79067362569e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Sauk (ftUS)
    # (7631, -92.168857, 42.796962, 1e-4, 0.0, 0.0, 9.4258201226e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Sawyer (ftUS)
    # (7632, -93.854626, 44.780433, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Shawano (ftUS)
    # (7633, -91.876779, 43.989259, 1e-4, 0.0, 0.0, 7.61857662312e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS St. Croix (ftUS)
    # (7634, -94.697360, 44.017464, 1e-4, 0.0, 0.0, 5.48792333874e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Taylor (ftUS)
    # (7635, -92.823746, 44.183937, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Trempealeau (ftUS)
    # (7636, -94.523609, 43.117539, 1e-4, 0.0, 0.0, 6.22182217338e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Vernon (ftUS)
    # (7637, -93.517443, 43.114282, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Vilas (ftUS)
    # (7638, -91.212069, 45.611901, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Walworth (ftUS)
    # (7639, -91.332731, 41.634881, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Washburn (ftUS)
    # (7640, -94.712336, 44.227972, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Washington (ftUS)
    # (7641, -89.534418, 42.908610, 1e-4, 0.0, 0.0, 8.117046327e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Waukesha (ftUS)
    # (7642, -90.766546, 42.541250, 1e-4, 0.0, 0.0, 1.80133953408e-05),
    # NAD83(2011) - NAD83(2011) / WISCRS Waupaca (ftUS)
    # (7643, -91.100301, 43.397471, 1e-4, 0.0, 0.0, 9.77824085212e-06),
    # NAD83(2011) - NAD83(2011) / WISCRS Waushara (ftUS)
    # (7644, -90.731413, 43.698552, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83(2011) - NAD83(2011) / WISCRS Wood (ftUS)
    # (7645, -92.561588, 43.122063, 1e-4, 0.0, 0.0, 1e-07),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 4
    (20004, -16.408735, -0.000738, 1e-4, 0.0, 0.0, 2788),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 5
    (20005, -17.144950, -0.000738, 1e-4, 0.0, 0.0, 15649),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 6
    (20006, -17.069426, -0.000738, 1e-4, 0.0, 0.0, 66222),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 7
    (20007, -15.997884, -0.000738, 1e-4, 0.0, 0.0, 228895),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 8
    (20008, -13.542550, -0.000738, 1e-4, 0.0, 0.0, 675426),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 9
    (20009, -8.938427, -0.000738, 1e-4, 0.0, 0.0, 1736227),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 10
    (20010, -0.808512, -0.000738, 1e-4, 0.0, 0.0, 3900661),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 11
    (20011, 13.140164, -0.000738, 1e-4, 0.0, 0.0, 7620250),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 12
    (20012, 36.497864, -0.000738, 1e-4, 0.0, 0.0, 13003845.6944),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 13
    (20013, 74.618942, -0.000738, 1e-4, 0.0, 0.0, 20186451.1937),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 14
    (20014, 135.181910, -0.000738, 1e-4, 0.0, 0.0, 32540211.6008),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 15
    (20015, -131.150264, -0.000738, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 16
    (20016, 10.043713, -0.000738, 1e-4, 0.0, 0.0, 1897470),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 17
    (20017, -142.158529, -0.000738, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 18
    (20018, 156.971293, -0.000738, 1e-4, 0.0, 0.0, 37933969.7856),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 19
    (20019, -141.014850, -0.000738, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 20
    (20020, 83.538261, -0.000738, 1e-4, 0.0, 0.0, 24812506.305),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 21
    (20021, 159.842297, -0.000738, 1e-4, 0.0, 0.0, 38878499.3142),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 22
    (20022, 148.253039, -0.000738, 1e-4, 0.0, 0.0, 37027455.3704),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 23
    (20023, 122.053328, -0.000738, 1e-4, 0.0, 0.0, 33069293.0703),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 24
    (20024, 169.391601, -0.000738, 1e-4, 0.0, 0.0, 41698979.5416),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 25
    (20025, 35.403221, -0.000738, 1e-4, 0.0, 0.0, 0.926885433177),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 26
    (20026, -155.454538, -0.000738, 1e-4, 0.0, 0.0, 49820287.8768),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 27
    (20027, 103.088669, -0.000738, 1e-4, 0.0, 0.0, 29965077.9258),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 28
    (20028, -98.130871, -0.000738, 1e-4, 0.0, 0.0, 3),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 29
    (20029, 159.211369, -0.000738, 1e-4, 0.0, 0.0, 42267195.8384),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 30
    (20030, 24.096714, -0.000738, 1e-4, 0.0, 0.0, 0.630883813183),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 31
    (20031, 119.571544, -0.000738, 1e-4, 0.0, 0.0, 33569247.8905),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger zone 32
    (20032, 26.360878, -0.000738, 1e-4, 0.0, 0.0, 0.690159166837),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 4N (deprecated)
    (20064, 16.511970, -0.000738, 1e-4, 0.0, 0.0, 0.000171152612893),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 5N (deprecated)
    (20065, 22.511989, -0.000738, 1e-4, 0.0, 0.0, 0.000171152612893),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 6N (deprecated)
    (20066, 28.512022, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 7N (deprecated)
    (20067, 34.512066, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057636),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 8N (deprecated)
    (20068, 40.512123, -0.000738, 1e-4, 0.0, 0.0, 0.00017115479568),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 9N (deprecated)
    (20069, 46.512191, -0.000738, 1e-4, 0.0, 0.0, 0.00017115479568),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 10N (deprecated)
    (20070, 52.512270, -0.000738, 1e-4, 0.0, 0.0, 0.000171154795744),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 11N (deprecated)
    (20071, 58.512358, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866706),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 12N (deprecated)
    (20072, 64.512455, -0.000738, 1e-4, 0.0, 0.0, 0.000171151565176),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 13N (deprecated)
    (20073, 70.512560, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866706),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 14N (deprecated)
    (20074, 76.512672, -0.000738, 1e-4, 0.0, 0.0, 0.00017114877123),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 15N (deprecated)
    (20075, 82.512789, -0.000738, 1e-4, 0.0, 0.0, 0.000171150168171),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 16N (deprecated)
    (20076, 88.512910, -0.000738, 1e-4, 0.0, 0.0, 0.00017115226369),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 17N (deprecated)
    (20077, 94.513033, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660674),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 18N (deprecated)
    (20078, 100.513158, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 19N (deprecated)
    (20079, 106.513283, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 20N (deprecated)
    (20080, 112.513407, -0.000738, 1e-4, 0.0, 0.0, 0.000171148771209),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 21N (deprecated)
    (20081, 118.513528, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 22N (deprecated)
    (20082, 124.513645, -0.000738, 1e-4, 0.0, 0.0, 0.000171149469722),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 23N (deprecated)
    (20083, 130.513757, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 24N (deprecated)
    (20084, 136.513862, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660652),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 25N (deprecated)
    (20085, 142.513959, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866727),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 26N (deprecated)
    (20086, 148.514048, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057615),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 27N (deprecated)
    (20087, 154.514127, -0.000738, 1e-4, 0.0, 0.0, 0.000171153660674),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 28N (deprecated)
    (20088, 160.514195, -0.000738, 1e-4, 0.0, 0.0, 0.000171152263647),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 29N (deprecated)
    (20089, 166.514252, -0.000738, 1e-4, 0.0, 0.0, 0.000171155057636),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 30N (deprecated)
    (20090, 172.514297, -0.000738, 1e-4, 0.0, 0.0, 0.00017115226369),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 31N (deprecated)
    (20091, 178.514330, -0.000738, 1e-4, 0.0, 0.0, 0.000171145191416),
    # Pulkovo 1995 - Pulkovo 1995 / Gauss-Kruger 32N (deprecated)
    (20092, -175.485651, -0.000738, 1e-4, 0.0, 0.0, 0.000171150866684),
    # Adindan - Adindan / UTM zone 35N
    (20135, 22.511782, 0.001845, 1e-4, 0.0, 0.0, 0.00017212840612),
    # Adindan - Adindan / UTM zone 36N
    (20136, 28.511929, 0.001845, 1e-4, 0.0, 0.0, 0.000172129541255),
    # Adindan - Adindan / UTM zone 37N
    (20137, 34.512069, 0.001845, 1e-4, 0.0, 0.0, 0.000172126834514),
    # Adindan - Adindan / UTM zone 38N
    (20138, 40.512202, 0.001845, 1e-4, 0.0, 0.0, 0.000172129017344),
    # AGD66 - AGD66 / AMG zone 48 (deprecated)
    (20248, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 753000),
    # AGD66 - AGD66 / AMG zone 49
    (20249, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 753000),
    # AGD66 - AGD66 / AMG zone 50
    (20250, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 753000),
    # AGD66 - AGD66 / AMG zone 51
    (20251, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 753000),
    # AGD66 - AGD66 / AMG zone 52
    (20252, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 753000),
    # AGD66 - AGD66 / AMG zone 53
    (20253, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 753000),
    # AGD66 - AGD66 / AMG zone 54
    (20254, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 753000),
    # AGD66 - AGD66 / AMG zone 55
    (20255, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 4),
    # AGD66 - AGD66 / AMG zone 56
    (20256, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 4),
    # AGD66 - AGD66 / AMG zone 57
    (20257, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 4),
    # AGD66 - AGD66 / AMG zone 58
    (20258, -162.215428, -89.998763, 1e-4, 0.0, 0.0, 4),
    # AGD84 - AGD84 / AMG zone 48 (deprecated)
    (20348, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # AGD84 - AGD84 / AMG zone 49
    (20349, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # AGD84 - AGD84 / AMG zone 50
    (20350, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # AGD84 - AGD84 / AMG zone 51
    (20351, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # AGD84 - AGD84 / AMG zone 52
    (20352, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # AGD84 - AGD84 / AMG zone 53
    (20353, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # AGD84 - AGD84 / AMG zone 54
    (20354, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # AGD84 - AGD84 / AMG zone 55
    (20355, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # AGD84 - AGD84 / AMG zone 56
    (20356, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # AGD84 - AGD84 / AMG zone 57 (deprecated)
    (20357, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # AGD84 - AGD84 / AMG zone 58 (deprecated)
    (20358, -160.292021, -89.998726, 1e-4, 0.0, 0.0, 3),
    # Ain el Abd - Ain el Abd / UTM zone 36N
    (20436, 28.510183, 0.000063, 1e-4, 0.0, 0.0, 0.000168837199453),
    # Ain el Abd - Ain el Abd / UTM zone 37N
    (20437, 34.510413, 0.000063, 1e-4, 0.0, 0.0, 0.000168834143551),
    # Ain el Abd - Ain el Abd / UTM zone 38N
    (20438, 40.510655, 0.000063, 1e-4, 0.0, 0.0, 0.000168836675586),
    # Ain el Abd - Ain el Abd / UTM zone 39N
    (20439, 46.510905, 0.000063, 1e-4, 0.0, 0.0, 0.000168835278602),
    # Ain el Abd - Ain el Abd / UTM zone 40N
    (20440, 52.511162, 0.000063, 1e-4, 0.0, 0.0, 0.00016883667559),
    # Ain el Abd - Ain el Abd / Bahrain Grid
    (20499, 46.510905, 0.000063, 1e-4, 0.0, 0.0, 0.000168835278602),
    # Afgooye - Afgooye / UTM zone 38N
    (20538, 40.510470, 0.000407, 1e-4, 0.0, 0.0, 0.000167676043951),
    # Afgooye - Afgooye / UTM zone 39N
    (20539, 46.510604, 0.000407, 1e-4, 0.0, 0.0, 0.000167678837919),
    # Lisbon (Lisbon) - Lisbon (Lisbon) / Portuguese National Grid
    (20790, -10.378147, 36.944390, 1e-4, 0.0, 0.0, 2.58127693087e-06),
    # Lisbon (Lisbon) - Lisbon (Lisbon) / Portuguese Grid
    (20791, -8.133106, 39.668258, 1e-4, 0.0, 0.0, 1e-07),
    # Aratu - Aratu / UTM zone 22S
    (20822, 117.901581, -89.997092, 1e-4, 0.0, 0.0, 752569),
    # Aratu - Aratu / UTM zone 23S
    (20823, 117.901581, -89.997092, 1e-4, 0.0, 0.0, 752569),
    # Aratu - Aratu / UTM zone 24S
    (20824, 117.901581, -89.997092, 1e-4, 0.0, 0.0, 752569),
    # Arc 1950 - Arc 1950 / UTM zone 34S
    (20934, -147.814894, -89.998487, 1e-4, 0.0, 0.0, 753200),
    # Arc 1950 - Arc 1950 / UTM zone 35S
    (20935, -147.814894, -89.998487, 1e-4, 0.0, 0.0, 753200),
    # Arc 1950 - Arc 1950 / UTM zone 36S
    (20936, -147.814894, -89.998487, 1e-4, 0.0, 0.0, 753200),
    # Arc 1960 - Arc 1960 / UTM zone 35S
    (21035, -177.852415, -89.998567, 1e-4, 0.0, 0.0, 753200),
    # Arc 1960 - Arc 1960 / UTM zone 36S
    (21036, -177.852415, -89.998567, 1e-4, 0.0, 0.0, 753200),
    # Arc 1960 - Arc 1960 / UTM zone 37S
    (21037, -177.852415, -89.998567, 1e-4, 0.0, 0.0, 753200),
    # Arc 1960 - Arc 1960 / UTM zone 35N
    (21095, 22.511836, -0.002731, 1e-4, 0.0, 0.0, 0.000172129017301),
    # Arc 1960 - Arc 1960 / UTM zone 36N
    (21096, 28.511974, -0.002731, 1e-4, 0.0, 0.0, 0.00017212840612),
    # Arc 1960 - Arc 1960 / UTM zone 37N
    (21097, 34.512105, -0.002731, 1e-4, 0.0, 0.0, 0.000172126485268),
    # Batavia (Jakarta) - Batavia (Jakarta) / NEIEZ (deprecated)
    (21100, -178.342349, -8.136903, 1e-4, 0.0, 0.0, 1e-07),
    # Batavia - Batavia / UTM zone 48S
    (21148, 118.968811, -89.993030, 1e-4, 0.0, 0.0, 3),
    # Batavia - Batavia / UTM zone 49S
    (21149, 118.968811, -89.993030, 1e-4, 0.0, 0.0, 3),
    # Batavia - Batavia / UTM zone 50S
    (21150, 118.968811, -89.993030, 1e-4, 0.0, 0.0, 3),
    # Barbados 1938 - Barbados 1938 / British West Indies Grid
    (21291, -65.591244, 0.003791, 1e-4, 0.0, 0.0, 4.86146018375e-05),
    # Barbados 1938 - Barbados 1938 / Barbados National Grid
    (21292, -59.834063, 12.501095, 1e-4, 0.0, 0.0, 1e-07),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 13
    (21413, 74.618925, -0.000744, 1e-4, 0.0, 0.0, 20186451.1937),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 14
    (21414, 135.182079, -0.000744, 1e-4, 0.0, 0.0, 32540211.6008),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 15
    (21415, -131.150219, -0.000744, 1e-4, 0.0, 0.0, 4),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 16
    (21416, 10.043482, -0.000744, 1e-4, 0.0, 0.0, 1897470),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 17
    (21417, -142.158447, -0.000744, 1e-4, 0.0, 0.0, 4),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 18
    (21418, 156.971482, -0.000744, 1e-4, 0.0, 0.0, 37933969.7856),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 19
    (21419, -141.014771, -0.000744, 1e-4, 0.0, 0.0, 4),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 20
    (21420, 83.538279, -0.000744, 1e-4, 0.0, 0.0, 24812506.305),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 21
    (21421, 159.842486, -0.000744, 1e-4, 0.0, 0.0, 38878499.3142),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 22
    (21422, 148.253223, -0.000744, 1e-4, 0.0, 0.0, 37027455.3704),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger zone 23
    (21423, 122.053470, -0.000744, 1e-4, 0.0, 0.0, 33069293.0703),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 75E
    (21453, 70.512527, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 81E
    (21454, 76.512662, -0.000744, 1e-4, 0.0, 0.0, 0.000167353166099),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 87E
    (21455, 82.512802, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 93E
    (21456, 88.512946, -0.000744, 1e-4, 0.0, 0.0, 0.000167358055499),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 99E
    (21457, 94.513091, -0.000744, 1e-4, 0.0, 0.0, 0.000167353864526),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 105E
    (21458, 100.513237, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 111E
    (21459, 106.513382, -0.000744, 1e-4, 0.0, 0.0, 0.000167356658515),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 117E
    (21460, 112.513524, -0.000744, 1e-4, 0.0, 0.0, 0.000167351769072),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 123E
    (21461, 118.513661, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 129E
    (21462, 124.513793, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger CM 135E
    (21463, 130.513917, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 13N (deprecated)
    (21473, 70.512527, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 14N (deprecated)
    (21474, 76.512662, -0.000744, 1e-4, 0.0, 0.0, 0.000167353166099),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 15N (deprecated)
    (21475, 82.512802, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 16N (deprecated)
    (21476, 88.512946, -0.000744, 1e-4, 0.0, 0.0, 0.000167358055499),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 17N (deprecated)
    (21477, 94.513091, -0.000744, 1e-4, 0.0, 0.0, 0.000167353864526),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 18N (deprecated)
    (21478, 100.513237, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 19N (deprecated)
    (21479, 106.513382, -0.000744, 1e-4, 0.0, 0.0, 0.000167356658515),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 20N (deprecated)
    (21480, 112.513524, -0.000744, 1e-4, 0.0, 0.0, 0.000167351769072),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 21N (deprecated)
    (21481, 118.513661, -0.000744, 1e-4, 0.0, 0.0, 0.00016735526151),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 22N (deprecated)
    (21482, 124.513793, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # Beijing 1954 - Beijing 1954 / Gauss-Kruger 23N (deprecated)
    (21483, 130.513917, -0.000744, 1e-4, 0.0, 0.0, 0.000167355261532),
    # Belge 1950 (Brussels) - Belge 1950 (Brussels) / Belge Lambert 50
    (21500, 2.305957, 49.294707, 1e-4, 0.0, 0.0, 2.2268795874e-06),
    # Bern 1898 (Bern) - Bern 1898 (Bern) / LV03C
    (21780, 7.438633, 46.951082, 1e-4, 0.0, 0.0, 1e-07),
    # CH1903 - CH1903 / LV03
    (21781, -0.163873, 44.890217, 1e-4, 0.0, 0.0, 1e-07),
    # CH1903 - CH1903 / LV03C-G
    (21782, 7.438633, 46.951082, 1e-4, 0.0, 0.0, 1e-07),
    # Bogota 1975 - Bogota 1975 / UTM zone 17N (deprecated)
    (21817, -85.485603, -0.002876, 1e-4, 0.0, 0.0, 0.00016883667567),
    # Bogota 1975 - Bogota 1975 / UTM zone 18N
    (21818, -79.485358, -0.002876, 1e-4, 0.0, 0.0, 0.00016883667567),
    # Bogota 1975 - Bogota 1975 / Colombia West zone (deprecated)
    (21891, -86.050311, -4.392762, 1e-4, 0.0, 0.0, 0.0141848220374),
    # Bogota 1975 - Bogota 1975 / Colombia Bogota zone (deprecated)
    (21892, -83.050182, -4.392750, 1e-4, 0.0, 0.0, 0.0141848206404),
    # Bogota 1975 - Bogota 1975 / Colombia East Central zone (deprecated)
    (21893, -80.050061, -4.392737, 1e-4, 0.0, 0.0, 0.0141848232597),
    # Bogota 1975 - Bogota 1975 / Colombia East (deprecated)
    (21894, -77.049950, -4.392724, 1e-4, 0.0, 0.0, 0.0141848223866),
    # Bogota 1975 - Bogota 1975 / Colombia West zone
    (21896, -86.050311, -4.392762, 1e-4, 0.0, 0.0, 0.0141848220374),
    # Bogota 1975 - Bogota 1975 / Colombia Bogota zone
    (21897, -83.050182, -4.392750, 1e-4, 0.0, 0.0, 0.0141848206404),
    # Bogota 1975 - Bogota 1975 / Colombia East Central zone
    (21898, -80.050061, -4.392737, 1e-4, 0.0, 0.0, 0.0141848232597),
    # Bogota 1975 - Bogota 1975 / Colombia East
    (21899, -77.049950, -4.392724, 1e-4, 0.0, 0.0, 0.0141848223866),
    # Camacupa - Camacupa / UTM zone 32S
    (22032, -98.330767, -89.996855, 1e-4, 0.0, 0.0, 753200),
    # Camacupa - Camacupa / UTM zone 33S
    (22033, -98.330767, -89.996855, 1e-4, 0.0, 0.0, 753200),
    # Camacupa - Camacupa / TM 11.30 SE
    (22091, -98.330767, -89.996855, 1e-4, 0.0, 0.0, 753200),
    # Camacupa - Camacupa / TM 12 SE
    (22092, -98.330767, -89.996855, 1e-4, 0.0, 0.0, 753200),
    # POSGAR 98 - POSGAR 98 / Argentina 1
    (22171, -72.0, 90.0, 1e-4, 0.0, 0.0, 32255897.1877),
    # POSGAR 98 - POSGAR 98 / Argentina 2
    (22172, -69.0, 90.0, 1e-4, 0.0, 0.0, 33755897.1877),
    # POSGAR 98 - POSGAR 98 / Argentina 3
    (22173, -66.0, 90.0, 1e-4, 0.0, 0.0, 35255897.1877),
    # POSGAR 98 - POSGAR 98 / Argentina 4
    (22174, -63.0, 90.0, 1e-4, 0.0, 0.0, 36755897.1877),
    # POSGAR 98 - POSGAR 98 / Argentina 5
    (22175, -60.0, 90.0, 1e-4, 0.0, 0.0, 38255897.1877),
    # POSGAR 98 - POSGAR 98 / Argentina 6
    (22176, -57.0, 90.0, 1e-4, 0.0, 0.0, 39755897.1877),
    # POSGAR 98 - POSGAR 98 / Argentina 7
    (22177, -54.0, 90.0, 1e-4, 0.0, 0.0, 41255897.1877),
    # POSGAR 94 - POSGAR 94 / Argentina 1
    (22181, -72.0, 90.0, 1e-4, 0.0, 0.0, 32255897.1879),
    # POSGAR 94 - POSGAR 94 / Argentina 2
    (22182, -69.0, 90.0, 1e-4, 0.0, 0.0, 33755897.1879),
    # POSGAR 94 - POSGAR 94 / Argentina 3
    (22183, -66.0, 90.0, 1e-4, 0.0, 0.0, 35255897.1879),
    # POSGAR 94 - POSGAR 94 / Argentina 4
    (22184, -63.0, 90.0, 1e-4, 0.0, 0.0, 36755897.1879),
    # POSGAR 94 - POSGAR 94 / Argentina 5
    (22185, -60.0, 90.0, 1e-4, 0.0, 0.0, 38255897.1879),
    # POSGAR 94 - POSGAR 94 / Argentina 6
    (22186, -57.0, 90.0, 1e-4, 0.0, 0.0, 39755897.1879),
    # POSGAR 94 - POSGAR 94 / Argentina 7
    (22187, -54.0, 90.0, 1e-4, 0.0, 0.0, 41255897.1879),
    # Campo Inchauspe - Campo Inchauspe / Argentina 1
    (22191, 137.419509, 89.998201, 1e-4, 0.0, 0.0, 32256864.897),
    # Campo Inchauspe - Campo Inchauspe / Argentina 2
    (22192, 137.419509, 89.998201, 1e-4, 0.0, 0.0, 33756864.897),
    # Campo Inchauspe - Campo Inchauspe / Argentina 3
    (22193, 137.419509, 89.998201, 1e-4, 0.0, 0.0, 35256864.897),
    # Campo Inchauspe - Campo Inchauspe / Argentina 4
    (22194, 137.419509, 89.998201, 1e-4, 0.0, 0.0, 36756864.897),
    # Campo Inchauspe - Campo Inchauspe / Argentina 5
    (22195, 137.419509, 89.998201, 1e-4, 0.0, 0.0, 38256864.897),
    # Campo Inchauspe - Campo Inchauspe / Argentina 6
    (22196, 137.419509, 89.998201, 1e-4, 0.0, 0.0, 39756864.897),
    # Campo Inchauspe - Campo Inchauspe / Argentina 7
    (22197, 137.419509, 89.998201, 1e-4, 0.0, 0.0, 41256864.897),
    # Cape - Cape / UTM zone 34S
    (22234, -141.546291, -89.998445, 1e-4, 0.0, 0.0, 753200),
    # Cape - Cape / UTM zone 35S
    (22235, -141.546291, -89.998445, 1e-4, 0.0, 0.0, 753200),
    # Cape - Cape / UTM zone 36S (deprecated)
    (22236, -141.546291, -89.998445, 1e-4, 0.0, 0.0, 753200),
    # Cape - Cape / Lo15
    (22275, 14.999379, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Cape - Cape / Lo17
    (22277, 16.999429, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Cape - Cape / Lo19
    (22279, 18.999480, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Cape - Cape / Lo21
    (22281, 20.999532, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Cape - Cape / Lo23
    (22283, 22.999584, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Cape - Cape / Lo25
    (22285, 24.999637, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Cape - Cape / Lo27
    (22287, 26.999690, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Cape - Cape / Lo29
    (22289, 28.999744, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Cape - Cape / Lo31
    (22291, 30.999798, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Cape - Cape / Lo33
    (22293, 32.999852, -0.002641, 1e-4, 0.0, 0.0, 1e-07),
    # Carthage - Carthage / UTM zone 32N
    (22332, 4.511575, 0.003898, 1e-4, 0.0, 0.0, 0.000172127096619),
    # Carthage - Carthage / Nord Tunisie
    (22391, 4.541510, 33.173369, 1e-4, 0.0, 0.0, 1e-07),
    # Carthage - Carthage / Sud Tunisie
    (22392, 4.695116, 30.483750, 1e-4, 0.0, 0.0, 1e-07),
    # Corrego Alegre 1970-72 - Corrego Alegre 1970-72 / UTM zone 21S
    (22521, 140.614499, -89.997619, 5e-1, 0.0, 0.0, 752569),
    # Corrego Alegre 1970-72 - Corrego Alegre 1970-72 / UTM zone 22S
    (22522, 140.614499, -89.997619, 5e-1, 0.0, 0.0, 752569),
    # Corrego Alegre 1970-72 - Corrego Alegre 1970-72 / UTM zone 23S
    (22523, 140.614499, -89.997619, 5e-1, 0.0, 0.0, 752569),
    # Corrego Alegre 1970-72 - Corrego Alegre 1970-72 / UTM zone 24S
    (22524, 140.614499, -89.997619, 5e-1, 0.0, 0.0, 752569),
    # Corrego Alegre 1970-72 - Corrego Alegre 1970-72 / UTM zone 25S
    (22525, 140.614499, -89.997619, 5e-1, 0.0, 0.0, 752569),
    # Deir ez Zor - Deir ez Zor / Levant Zone
    (22700, 34.181710, 31.902175, 1e-4, 0.0, 0.0, 1e-07),
    # Deir ez Zor - Deir ez Zor / Syria Lambert
    (22770, 34.181710, 31.902175, 1e-4, 0.0, 0.0, 1e-07),
    # Deir ez Zor - Deir ez Zor / Levant Stereographic
    (22780, 39.151376, 34.199575, 1e-4, 0.0, 0.0, 1e-07),
    # Douala - Douala / UTM zone 32N (deprecated)
    (22832, 4.511335, 0.0, 1e-4, 0.0, 0.0, 0.000172127096448),
    # Egypt 1907 - Egypt 1907 / Blue Belt
    (22991, 32.134874, 20.046900, 1e-4, 0.0, 0.0, 1.23519566841e-06),
    # Egypt 1907 - Egypt 1907 / Red Belt
    (22992, 25.029006, 22.577976, 1e-4, 0.0, 0.0, 0.000327135378029),
    # Egypt 1907 - Egypt 1907 / Purple Belt
    (22993, 19.894530, 28.011259, 1e-4, 0.0, 0.0, 0.00127185335441),
    # Egypt 1907 - Egypt 1907 / Extended Purple Belt
    (22994, 20.363225, 19.046974, 1e-4, 0.0, 0.0, 0.000240583322011),
    # ED50 - ED50 / UTM zone 28N
    (23028, -19.489658, -0.001094, 1e-4, 0.0, 0.0, 0.000168836675648),
    # ED50 - ED50 / UTM zone 29N
    (23029, -13.489606, -0.001094, 1e-4, 0.0, 0.0, 0.000168836675584),
    # ED50 - ED50 / UTM zone 30N
    (23030, -7.489542, -0.001094, 1e-4, 0.0, 0.0, 0.00016883554062),
    # ED50 - ED50 / UTM zone 31N
    (23031, -1.489468, -0.001094, 1e-4, 0.0, 0.0, 0.000168839469595),
    # ED50 - ED50 / UTM zone 32N
    (23032, 4.510616, -0.001094, 1e-4, 0.0, 0.0, 0.000168837897967),
    # ED50 - ED50 / UTM zone 33N
    (23033, 10.510710, -0.001094, 1e-4, 0.0, 0.0, 0.000168839469595),
    # ED50 - ED50 / UTM zone 34N
    (23034, 16.510811, -0.001094, 1e-4, 0.0, 0.0, 0.000168835802491),
    # ED50 - ED50 / UTM zone 35N
    (23035, 22.510918, -0.001094, 1e-4, 0.0, 0.0, 0.000168836675606),
    # ED50 - ED50 / UTM zone 36N
    (23036, 28.511032, -0.001094, 1e-4, 0.0, 0.0, 0.000168835278622),
    # ED50 - ED50 / UTM zone 37N
    (23037, 34.511150, -0.001094, 1e-4, 0.0, 0.0, 0.000168836675584),
    # ED50 - ED50 / UTM zone 38N
    (23038, 40.511271, -0.001094, 1e-4, 0.0, 0.0, 0.00016883667567),
    # ED50 - ED50 / TM 0 N
    (23090, -4.489506, -0.001094, 1e-4, 0.0, 0.0, 0.000168835104042),
    # ED50 - ED50 / TM 5 NE
    (23095, 0.510559, -0.001094, 1e-4, 0.0, 0.0, 0.000168839294972),
    # Fahud - Fahud / UTM zone 39N
    (23239, 46.513592, 0.002086, 1e-4, 0.0, 0.0, 0.00024114586995),
    # Fahud - Fahud / UTM zone 40N
    (23240, 52.513803, 0.002086, 1e-4, 0.0, 0.0, 0.00024114586995),
    # Garoua - Garoua / UTM zone 33N (deprecated)
    (23433, 10.511335, 0.0, 1e-4, 0.0, 0.0, 0.000172127096448),
    # HD72 - HD72 / EOV
    (23700, 10.787670, 45.035856, 1e-4, 0.0, 0.0, 1e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 46.2
    (23830, 92.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 47.1
    (23831, 95.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 47.2
    (23832, 98.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 48.1
    (23833, 101.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 48.2
    (23834, 104.652313, -13.557546, 1e-4, 0.0, 0.0, 7.0093665272e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 49.1
    (23835, 107.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 49.2
    (23836, 110.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 50.1
    (23837, 113.652313, -13.557546, 1e-4, 0.0, 0.0, 7.0093665272e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 50.2
    (23838, 116.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 51.1
    (23839, 119.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 51.2
    (23840, 122.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 52.1
    (23841, 125.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 52.2
    (23842, 128.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 53.1
    (23843, 131.652313, -13.557546, 1e-4, 0.0, 0.0, 6.98884832673e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 53.2
    (23844, 134.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # DGN95 - DGN95 / Indonesia TM-3 zone 54.1
    (23845, 137.652313, -13.557546, 1e-4, 0.0, 0.0, 7.02988472767e-07),
    # ID74 - ID74 / UTM zone 46N
    (23846, 88.511484, 0.000045, 1e-4, 0.0, 0.0, 0.000167735241121),
    # ID74 - ID74 / UTM zone 47N
    (23847, 94.511498, 0.000045, 1e-4, 0.0, 0.0, 0.000167736638107),
    # ID74 - ID74 / UTM zone 48N
    (23848, 100.511509, 0.000045, 1e-4, 0.0, 0.0, 0.000167734542631),
    # ID74 - ID74 / UTM zone 49N
    (23849, 106.511517, 0.000045, 1e-4, 0.0, 0.0, 0.000167738035092),
    # ID74 - ID74 / UTM zone 50N
    (23850, 112.511523, 0.000045, 1e-4, 0.0, 0.0, 0.000167734542631),
    # ID74 - ID74 / UTM zone 51N
    (23851, 118.511526, 0.000045, 1e-4, 0.0, 0.0, 0.00016773454263),
    # ID74 - ID74 / UTM zone 52N
    (23852, 124.511526, 0.000045, 1e-4, 0.0, 0.0, 0.000167733145646),
    # ID74 - ID74 / UTM zone 53N (deprecated)
    (23853, 130.511524, 0.000045, 1e-4, 0.0, 0.0, 0.000167734542629),
    # DGN95 - DGN95 / UTM zone 46N
    (23866, 88.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # DGN95 - DGN95 / UTM zone 47N
    (23867, 94.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # DGN95 - DGN95 / UTM zone 48N
    (23868, 100.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167725549545),
    # DGN95 - DGN95 / UTM zone 49N
    (23869, 106.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # DGN95 - DGN95 / UTM zone 50N
    (23870, 112.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # DGN95 - DGN95 / UTM zone 51N
    (23871, 118.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # DGN95 - DGN95 / UTM zone 52N
    (23872, 124.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # DGN95 - DGN95 / UTM zone 47S
    (23877, 99.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # DGN95 - DGN95 / UTM zone 48S
    (23878, 105.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # DGN95 - DGN95 / UTM zone 49S
    (23879, 111.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # DGN95 - DGN95 / UTM zone 50S
    (23880, 117.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # DGN95 - DGN95 / UTM zone 51S
    (23881, 123.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # DGN95 - DGN95 / UTM zone 52S
    (23882, 129.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # DGN95 - DGN95 / UTM zone 53S
    (23883, 135.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # DGN95 - DGN95 / UTM zone 54S
    (23884, 141.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # ID74 - ID74 / UTM zone 46S (deprecated)
    (23886, -147.994617, -89.999747, 1e-4, 0.0, 0.0, 3),
    # ID74 - ID74 / UTM zone 47S
    (23887, -147.994617, -89.999747, 1e-4, 0.0, 0.0, 3),
    # ID74 - ID74 / UTM zone 48S
    (23888, -147.994617, -89.999747, 1e-4, 0.0, 0.0, 3),
    # ID74 - ID74 / UTM zone 49S
    (23889, -147.994617, -89.999747, 1e-4, 0.0, 0.0, 3),
    # ID74 - ID74 / UTM zone 50S
    (23890, -147.994617, -89.999747, 1e-4, 0.0, 0.0, 3),
    # ID74 - ID74 / UTM zone 51S
    (23891, -147.994617, -89.999747, 1e-4, 0.0, 0.0, 3),
    # ID74 - ID74 / UTM zone 52S
    (23892, -147.994617, -89.999747, 1e-4, 0.0, 0.0, 3),
    # ID74 - ID74 / UTM zone 53S
    (23893, -147.994617, -89.999747, 1e-4, 0.0, 0.0, 3),
    # ID74 - ID74 / UTM zone 54S
    (23894, -147.994617, -89.999747, 1e-4, 0.0, 0.0, 3),
    # Indian 1954 - Indian 1954 / UTM zone 46N
    (23946, 88.508895, 0.002704, 1e-4, 0.0, 0.0, 0.000165573495991),
    # Indian 1954 - Indian 1954 / UTM zone 47N
    (23947, 94.508127, 0.002704, 1e-4, 0.0, 0.0, 0.000165569828823),
    # Indian 1954 - Indian 1954 / UTM zone 48N
    (23948, 100.507386, 0.002704, 1e-4, 0.0, 0.0, 0.000165572098922),
    # Indian 1975 - Indian 1975 / UTM zone 47N
    (24047, 94.508196, 0.002614, 1e-4, 0.0, 0.0, 0.000165572099007),
    # Indian 1975 - Indian 1975 / UTM zone 48N
    (24048, 100.507463, 0.002614, 1e-4, 0.0, 0.0, 0.000165573495906),
    # Jamaica 1875 - Jamaica 1875 / Jamaica (Old Grid)
    (24100, -78.573073, 16.891955, 1e-4, 0.0, 0.0, 5.84803178593e-07),
    # JAD69 - JAD69 / Jamaica National Grid
    (24200, -79.341549, 16.633127, 1e-4, 0.0, 0.0, 1.66503014043e-07),
    # Kalianpur 1937 - Kalianpur 1937 / UTM zone 45N
    (24305, 82.509687, 0.002424, 1e-4, 0.0, 0.0, 0.000165573495991),
    # Kalianpur 1937 - Kalianpur 1937 / UTM zone 46N
    (24306, 88.508917, 0.002424, 1e-4, 0.0, 0.0, 0.000165576290045),
    # Kalianpur 1962 - Kalianpur 1962 / UTM zone 41N
    (24311, 58.511701, 0.002089, 1e-4, 0.0, 0.0, 0.00016557375784),
    # Kalianpur 1962 - Kalianpur 1962 / UTM zone 42N
    (24312, 64.511011, 0.002089, 1e-4, 0.0, 0.0, 0.000165571662493),
    # Kalianpur 1962 - Kalianpur 1962 / UTM zone 43N
    (24313, 70.510316, 0.002089, 1e-4, 0.0, 0.0, 0.00016557166245),
    # Kalianpur 1975 - Kalianpur 1975 / UTM zone 42N
    (24342, 64.511121, 0.002324, 1e-4, 0.0, 0.0, 0.000165569304954),
    # Kalianpur 1975 - Kalianpur 1975 / UTM zone 43N
    (24343, 70.510375, 0.002324, 1e-4, 0.0, 0.0, 0.000165570701938),
    # Kalianpur 1975 - Kalianpur 1975 / UTM zone 44N
    (24344, 76.509633, 0.002324, 1e-4, 0.0, 0.0, 0.000165572710102),
    # Kalianpur 1975 - Kalianpur 1975 / UTM zone 45N
    (24345, 82.508902, 0.002324, 1e-4, 0.0, 0.0, 0.000165572710273),
    # Kalianpur 1975 - Kalianpur 1975 / UTM zone 46N
    (24346, 88.508190, 0.002324, 1e-4, 0.0, 0.0, 0.000165569304954),
    # Kalianpur 1975 - Kalianpur 1975 / UTM zone 47N
    (24347, 94.507505, 0.002324, 1e-4, 0.0, 0.0, 0.000165572099007),
    # Kalianpur 1880 - Kalianpur 1880 / India zone 0
    (24370, 49.079871, 16.627043, 1e-4, 0.0, 0.0, 2.03956304933e-07),
    # Kalianpur 1880 - Kalianpur 1880 / India zone I
    (24371, 41.760147, 21.241917, 1e-4, 0.0, 0.0, 1.82185688114e-07),
    # Kalianpur 1880 - Kalianpur 1880 / India zone IIa
    (24372, 48.679139, 15.379215, 1e-4, 0.0, 0.0, 1.89251590064e-07),
    # Kalianpur 1880 - Kalianpur 1880 / India zone IIIa
    (24373, 55.303442, 9.029679, 1e-4, 0.0, 0.0, 1e-07),
    # Kalianpur 1880 - Kalianpur 1880 / India zone IVa
    (24374, 55.587457, 2.661510, 1e-4, 0.0, 0.0, 6.75118285767e-06),
    # Kalianpur 1937 - Kalianpur 1937 / India zone IIb
    (24375, 64.680540, 15.380436, 1e-4, 0.0, 0.0, 1.79337803274e-07),
    # Kalianpur 1962 - Kalianpur 1962 / India zone I
    (24376, 41.763232, 21.242802, 1e-4, 0.0, 0.0, 1.72527506948e-07),
    # Kalianpur 1962 - Kalianpur 1962 / India zone IIa
    (24377, 48.681354, 15.380399, 1e-4, 0.0, 0.0, 1.67812686414e-07),
    # Kalianpur 1975 - Kalianpur 1975 / India zone I
    (24378, 41.763543, 21.242874, 1e-4, 0.0, 0.0, 1.73225998878e-07),
    # Kalianpur 1975 - Kalianpur 1975 / India zone IIa
    (24379, 48.681602, 15.380509, 1e-4, 0.0, 0.0, 1.74099113792e-07),
    # Kalianpur 1975 - Kalianpur 1975 / India zone IIb
    (24380, 64.679587, 15.380404, 1e-4, 0.0, 0.0, 1.76194589585e-07),
    # Kalianpur 1975 - Kalianpur 1975 / India zone IIIa
    (24381, 55.305046, 9.031390, 1e-4, 0.0, 0.0, 1e-07),
    # Kalianpur 1880 - Kalianpur 1880 / India zone IIb
    (24382, 64.679139, 15.379215, 1e-4, 0.0, 0.0, 1.89251590064e-07),
    # Kalianpur 1975 - Kalianpur 1975 / India zone IVa
    (24383, 55.589009, 2.663660, 1e-4, 0.0, 0.0, 6.18759077042e-06),
    # Kertau 1968 - Kertau 1968 / Singapore Grid
    (24500, 103.581731, 1.016276, 1e-4, 0.0, 0.0, 1e-07),
    # Kertau 1968 - Kertau 1968 / UTM zone 47N
    (24547, 94.510168, 0.000045, 1e-4, 0.0, 0.0, 0.000165570963875),
    # Kertau 1968 - Kertau 1968 / UTM zone 48N
    (24548, 100.509373, 0.000045, 1e-4, 0.0, 0.0, 0.000165572360858),
    # Kertau 1968 - Kertau / R.S.O. Malaya (ch) (deprecated)
    (24571, 98.017232, -0.009416, 1e-4, 0.0, 0.0, 1.52859086174e-07),
    # KOC - KOC Lambert
    (24600, 30.772130, 21.132169, 1e-4, 0.0, 0.0, 1.82655639946e-07),
    # La Canoa - La Canoa / UTM zone 18N
    (24718, -79.490802, -0.003237, 1e-4, 0.0, 0.0, 0.000168835278686),
    # La Canoa - La Canoa / UTM zone 19N
    (24719, -73.490641, -0.003237, 1e-4, 0.0, 0.0, 0.00016883667567),
    # La Canoa - La Canoa / UTM zone 20N
    (24720, -67.490457, -0.003237, 1e-4, 0.0, 0.0, 0.000168839469638),
    # PSAD56 - PSAD56 / UTM zone 17N
    (24817, -85.491023, -0.003400, 1e-4, 0.0, 0.0, 0.000168838072739),
    # PSAD56 - PSAD56 / UTM zone 18N
    (24818, -79.490824, -0.003400, 1e-4, 0.0, 0.0, 0.000168837374076),
    # PSAD56 - PSAD56 / UTM zone 19N
    (24819, -73.490601, -0.003400, 1e-4, 0.0, 0.0, 0.000168835278686),
    # PSAD56 - PSAD56 / UTM zone 20N
    (24820, -67.490356, -0.003400, 1e-4, 0.0, 0.0, 0.00016883667567),
    # PSAD56 - PSAD56 / UTM zone 21N
    (24821, -61.490090, -0.003400, 1e-4, 0.0, 0.0, 0.000168836675584),
    # PSAD56 - PSAD56 / UTM zone 17S
    (24877, 148.715507, -89.996983, 1e-4, 0.0, 0.0, 752569),
    # PSAD56 - PSAD56 / UTM zone 18S
    (24878, 148.715507, -89.996983, 1e-4, 0.0, 0.0, 752569),
    # PSAD56 - PSAD56 / UTM zone 19S
    (24879, 148.715507, -89.996983, 1e-4, 0.0, 0.0, 752569),
    # PSAD56 - PSAD56 / UTM zone 20S
    (24880, 148.715507, -89.996983, 1e-4, 0.0, 0.0, 752569),
    # PSAD56 - PSAD56 / UTM zone 21S
    (24881, 148.715507, -89.996983, 1e-4, 0.0, 0.0, 752569),
    # PSAD56 - PSAD56 / UTM zone 22S
    (24882, 148.715507, -89.996983, 1e-4, 0.0, 0.0, 752569),
    # PSAD56 - PSAD56 / Peru west zone
    (24891, -82.609400, -18.890747, 1e-4, 0.0, 0.0, 4.90123056807e-07),
    # PSAD56 - PSAD56 / Peru central zone
    (24892, -82.823136, -18.783737, 1e-4, 0.0, 0.0, 0.000315493438393),
    # PSAD56 - PSAD56 / Peru east zone
    (24893, -82.964923, -18.490582, 1e-4, 0.0, 0.0, 0.0831472506397),
    # Leigon - Leigon / Ghana Metre Grid
    (25000, -3.472002, 4.665180, 1e-4, 0.0, 0.0, 6.11742995008e-06),
    # Lome - Lome / UTM zone 31N
    (25231, -1.488665, 0.0, 1e-4, 0.0, 0.0, 0.000172127271071),
    # Luzon 1911 - Luzon 1911 / Philippines zone I
    (25391, 112.514242, -0.000461, 1e-4, 0.0, 0.0, 0.000170430634203),
    # Luzon 1911 - Luzon 1911 / Philippines zone II
    (25392, 114.514247, -0.000461, 1e-4, 0.0, 0.0, 0.000170430634181),
    # Luzon 1911 - Luzon 1911 / Philippines zone III
    (25393, 116.514251, -0.000461, 1e-4, 0.0, 0.0, 0.000170430634181),
    # Luzon 1911 - Luzon 1911 / Philippines zone IV
    (25394, 118.514253, -0.000461, 1e-4, 0.0, 0.0, 0.000170433428149),
    # Luzon 1911 - Luzon 1911 / Philippines zone V
    (25395, 120.514254, -0.000461, 1e-4, 0.0, 0.0, 0.000170432031165),
    # Makassar (Jakarta) - Makassar (Jakarta) / NEIEZ (deprecated)
    (25700, -178.340942, -8.134875, 1e-4, 0.0, 0.0, 1e-07),
    # ETRS89 - ETRS89 / UTM zone 28N
    (25828, -19.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 29N
    (25829, -13.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 30N
    (25830, -7.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # ETRS89 - ETRS89 / UTM zone 31N
    (25831, -1.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # ETRS89 - ETRS89 / UTM zone 32N
    (25832, 4.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # ETRS89 - ETRS89 / UTM zone 33N
    (25833, 10.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728605447),
    # ETRS89 - ETRS89 / UTM zone 34N
    (25834, 16.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 35N
    (25835, 22.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 36N
    (25836, 28.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 37N
    (25837, 34.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / UTM zone 38N (deprecated)
    (25838, 40.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # ETRS89 - ETRS89 / TM Baltic93
    (25884, 19.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # Malongo 1987 - Malongo 1987 / UTM zone 32S
    (25932, -178.791579, -89.997725, 1e-4, 0.0, 0.0, 752569),
    # Merchich - Merchich / Nord Maroc
    (26191, -10.603617, 30.479551, 1e-4, 0.0, 0.0, 1e-07),
    # Merchich - Merchich / Sud Maroc
    (26192, -10.428787, 26.892569, 1e-4, 0.0, 0.0, 1e-07),
    # Merchich - Merchich / Sahara (deprecated)
    (26193, -17.010608, 22.005023, 1e-4, 0.0, 0.0, 1.66590325534e-07),
    # Merchich - Merchich / Sahara Nord
    (26194, -17.010425, 22.005098, 1e-4, 0.0, 0.0, 1.72178260982e-07),
    # Merchich - Merchich / Sahara Sud
    (26195, -19.569955, 18.245743, 1e-4, 0.0, 0.0, 1.91124854609e-07),
    # Massawa - Massawa / UTM zone 37N
    (26237, 34.510482, 0.000543, 1e-4, 0.0, 0.0, 0.000167020509284),
    # Minna - Minna / UTM zone 31N
    (26331, -1.489521, 0.001103, 1e-4, 0.0, 0.0, 0.000172127009158),
    # Minna - Minna / UTM zone 32N
    (26332, 4.510568, 0.001103, 1e-4, 0.0, 0.0, 0.000172131200088),
    # Minna - Minna / Nigeria West Belt
    (26391, 2.421392, 3.998084, 1e-4, 0.0, 0.0, 2.50010793051e-06),
    # Minna - Minna / Nigeria Mid Belt
    (26392, 2.470781, 3.978525, 1e-4, 0.0, 0.0, 0.000998533445128),
    # Minna - Minna / Nigeria East Belt
    (26393, 2.549049, 3.940341, 1e-4, 0.0, 0.0, 0.0307079148212),
    # Mhast - Mhast / UTM zone 32S (deprecated)
    (26432, -179.069125, -89.997735, 1e-4, 0.0, 0.0, 752569),
    # Monte Mario (Rome) - Monte Mario (Rome) / Italy zone 1 (deprecated)
    (26591, -4.356422, 0.000703, 1e-4, 0.0, 0.0, 0.295115294857),
    # Monte Mario (Rome) - Monte Mario (Rome) / Italy zone 1 (deprecated)
    (26591, -4.356422, 0.000703, 1e-4, 0.0, 0.0, 0.295115294857),
    # Monte Mario (Rome) - Monte Mario (Rome) / Italy zone 2 (deprecated)
    (26592, -7.074640, 0.000687, 1e-4, 0.0, 0.0, 21),
    # Monte Mario (Rome) - Monte Mario (Rome) / Italy zone 2 (deprecated)
    (26592, -7.074640, 0.000687, 1e-4, 0.0, 0.0, 21),
    # M'poraloko - M'poraloko / UTM zone 32N
    (26632, 4.510224, 0.000380, 1e-4, 0.0, 0.0, 0.000172127096448),
    # M'poraloko - M'poraloko / UTM zone 32S
    (26692, -119.649864, -89.998661, 1e-4, 0.0, 0.0, 753200),
    # NAD27 - NAD27 / UTM zone 1N
    (26701, 178.511305, 0.0, 1e-4, 0.0, 0.0, 0.000170707935467),
    # NAD27 - NAD27 / UTM zone 2N
    (26702, -175.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170716491994),
    # NAD27 - NAD27 / UTM zone 3N
    (26703, -169.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170720682945),
    # NAD27 - NAD27 / UTM zone 4N
    (26704, -163.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170716491994),
    # NAD27 - NAD27 / UTM zone 5N
    (26705, -157.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170716491994),
    # NAD27 - NAD27 / UTM zone 6N
    (26706, -151.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170720682945),
    # NAD27 - NAD27 / UTM zone 7N
    (26707, -145.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170716491994),
    # NAD27 - NAD27 / UTM zone 8N
    (26708, -139.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170716491994),
    # NAD27 - NAD27 / UTM zone 9N
    (26709, -133.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170716491994),
    # NAD27 - NAD27 / UTM zone 10N
    (26710, -127.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170716491994),
    # NAD27 - NAD27 / UTM zone 11N
    (26711, -121.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170716491994),
    # NAD27 - NAD27 / UTM zone 12N
    (26712, -115.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27 - NAD27 / UTM zone 13N
    (26713, -109.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27 - NAD27 / UTM zone 14N
    (26714, -103.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27 - NAD27 / UTM zone 15N
    (26715, -97.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27 - NAD27 / UTM zone 16N
    (26716, -91.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170720682945),
    # NAD27 - NAD27 / UTM zone 17N
    (26717, -85.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27 - NAD27 / UTM zone 18N
    (26718, -79.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170720682945),
    # NAD27 - NAD27 / UTM zone 19N
    (26719, -73.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27 - NAD27 / UTM zone 20N
    (26720, -67.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27 - NAD27 / UTM zone 21N
    (26721, -61.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170718587469),
    # NAD27 - NAD27 / UTM zone 22N
    (26722, -55.488695, 0.0, 1e-4, 0.0, 0.0, 0.000170717539731),
    # NAD27 - NAD27 / Alabama East
    (26729, -87.420629, 30.490336, 2e-4, 0.0, 0.0, 9.71503110165e-07),
    # NAD27 - NAD27 / Alabama West
    (26730, -89.079325, 29.990526, 3e-4, 0.0, 0.0, 9.15065253807e-07),
    # NAD27 - NAD27 / Alaska zone 1
    (26731, -145.373446, 51.235712, 2e-3, 0.0, 0.0, 6.1324168928e-06),
    # NAD27 - NAD27 / Alaska zone 2
    (26732, -144.323130, 53.977542, 2e-3, 0.0, 0.0, 9.52055671802e-06),
    # NAD27 - NAD27 / Alaska zone 3
    (26733, -148.323130, 53.977542, 2e-3, 0.0, 0.0, 9.51252006428e-06),
    # NAD27 - NAD27 / Alaska zone 4
    (26734, -152.323130, 53.977542, 2e-3, 0.0, 0.0, 9.52055671802e-06),
    # NAD27 - NAD27 / Alaska zone 5
    (26735, -156.323130, 53.977542, 3e-3, 0.0, 0.0, 9.52055671802e-06),
    # NAD27 - NAD27 / Alaska zone 6
    (26736, -160.323130, 53.977542, 2e-3, 0.0, 0.0, 9.52055671802e-06),
    # NAD27 - NAD27 / Alaska zone 7
    (26737, -165.250967, 53.956004, 2e-3, 0.0, 0.0, 6.57096153414e-05),
    # NAD27 - NAD27 / Alaska zone 8
    (26738, -168.323130, 53.977542, 2e-3, 0.0, 0.0, 9.51252006428e-06),
    # NAD27 - NAD27 / Alaska zone 9
    (26739, -172.787200, 53.967668, 3e-3, 0.0, 0.0, 2.72915133485e-05),
    # NAD27 - NAD27 / Alaska zone 10
    (26740, 171.118573, 50.262081, 1e-4, 0.0, 0.0, 7.27418915467e-06),
    # NAD27 - NAD27 / California zone I
    (26741, -129.053312, 39.112276, 2e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / California zone II
    (26742, -128.894411, 37.458275, 2e-3, 0.0, 0.0, 1.6364580872e-07),
    # NAD27 - NAD27 / California zone III
    (26743, -127.291028, 36.300627, 2e-3, 0.0, 0.0, 1.96107888754e-07),
    # NAD27 - NAD27 / California zone IV
    (26744, -125.692769, 35.141841, 2e-3, 0.0, 0.0, 2.00785955347e-07),
    # NAD27 - NAD27 / California zone V
    (26745, -124.549846, 33.320893, 2e-3, 0.0, 0.0, 2.4317707747e-07),
    # NAD27 - NAD27 / California zone VI
    (26746, -122.703568, 31.996506, 2e-3, 0.0, 0.0, 2.77452679584e-07),
    # NAD27 - NAD27 / California zone VII (deprecated)
    (26747, -131.903082, 32.221959, 2e-3, 0.0, 0.0, 2.99559742416e-07),
    # NAD27 - NAD27 / Arizona East
    (26748, -111.762275, 30.990142, 7e-4, 0.0, 0.0, 1.02763225759e-06),
    # NAD27 - NAD27 / Arizona Central
    (26749, -113.512275, 30.990142, 8e-4, 0.0, 0.0, 1.02763225759e-06),
    # NAD27 - NAD27 / Arizona West
    (26750, -115.345555, 30.990143, 8e-4, 0.0, 0.0, 1.03934068305e-06),
    # NAD27 - NAD27 / Arkansas North
    (26751, -98.613238, 34.148745, 4e-4, 0.0, 0.0, 2.20549244152e-07),
    # NAD27 - NAD27 / Arkansas South
    (26752, -98.488589, 32.492435, 4e-4, 0.0, 0.0, 2.58835208584e-07),
    # NAD27 - NAD27 / Colorado North
    (26753, -112.554726, 39.114818, 8e-4, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Colorado Central
    (26754, -112.410094, 37.624391, 8e-4, 0.0, 0.0, 1.65364535428e-07),
    # NAD27 - NAD27 / Colorado South
    (26755, -112.305641, 36.466493, 8e-4, 0.0, 0.0, 1.77585213127e-07),
    # NAD27 - NAD27 / Connecticut
    (26756, -74.917560, 40.812679, 4e-4, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Delaware
    (26757, -77.151391, 37.987204, 1e-3, 0.0, 0.0, 2.03253984491e-06),
    # NAD27 - NAD27 / Florida East
    (26758, -82.501519, 24.325905, 1e-3, 0.0, 0.0, 3.33725586717e-07),
    # NAD27 - NAD27 / Florida West
    (26759, -83.501519, 24.325905, 1e-3, 0.0, 0.0, 3.33725586717e-07),
    # NAD27 - NAD27 / Florida North
    (26760, -90.748889, 28.849261, 1e-3, 0.0, 0.0, 3.70923352741e-07),
    # NAD27 - NAD27 / Georgia East
    (26766, -83.746044, 29.990526, 1e-3, 0.0, 0.0, 9.15697372682e-07),
    # NAD27 - NAD27 / Georgia West
    (26767, -85.746044, 29.990526, 1e-3, 0.0, 0.0, 9.15697372682e-07),
    # NAD27 - NAD27 / Idaho East
    (26768, -113.996200, 41.652103, 1e-3, 0.0, 0.0, 2.81150534635e-06),
    # NAD27 - NAD27 / Idaho Central
    (26769, -115.829534, 41.652103, 1e-3, 0.0, 0.0, 2.81150534635e-06),
    # NAD27 - NAD27 / Idaho West
    (26770, -117.579559, 41.652103, 1e-3, 0.0, 0.0, 2.81924724698e-06),
    # NAD27 - NAD27 / Illinois East
    (26771, -90.037719, 36.654470, 1e-3, 0.0, 0.0, 1.80642312503e-06),
    # NAD27 - NAD27 / Illinois West
    (26772, -91.871110, 36.654469, 1e-3, 0.0, 0.0, 1.8097987086e-06),
    # NAD27 - NAD27 / Indiana East
    (26773, -87.389814, 37.487431, 1e-3, 0.0, 0.0, 1.94929340163e-06),
    # NAD27 - NAD27 / Indiana West
    (26774, -88.806481, 37.487431, 1e-3, 0.0, 0.0, 1.94395200813e-06),
    # NAD27 - NAD27 / Iowa North
    (26775, -100.781587, 41.263510, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Iowa South
    (26776, -100.621376, 39.775170, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Kansas North
    (26777, -104.957693, 38.122270, 1e-3, 0.0, 0.0, 1.50279313216e-07),
    # NAD27 - NAD27 / Kansas South
    (26778, -105.305455, 36.466127, 1e-3, 0.0, 0.0, 1.80449757639e-07),
    # NAD27 - NAD27 / Kentucky North
    (26779, -91.130249, 37.294778, 1e-3, 0.0, 0.0, 1.50947017491e-07),
    # NAD27 - NAD27 / Kentucky South
    (26780, -92.527270, 36.136221, 1e-3, 0.0, 0.0, 1.8455491533e-07),
    # NAD27 - NAD27 / Louisiana North
    (26781, -98.852579, 30.505480, 1e-3, 0.0, 0.0, 3.19749006334e-07),
    # NAD27 - NAD27 / Louisiana South
    (26782, -97.562196, 28.517162, 1e-3, 0.0, 0.0, 3.59370379317e-07),
    # NAD27 - NAD27 / Maine East
    (26783, -70.394434, 43.817629, 1e-3, 0.0, 0.0, 3.41814271849e-06),
    # NAD27 - NAD27 / Maine West
    (26784, -72.030168, 42.818167, 1e-3, 0.0, 0.0, 3.12163023255e-06),
    # NAD27 - NAD27 / Maryland
    (26785, -79.768709, 37.800019, 1e-3, 0.0, 0.0, 1.52808971424e-07),
    # NAD27 - NAD27 / Massachusetts Mainland
    (26786, -73.672725, 40.979028, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Massachusetts Island
    (26787, -71.224498, 40.997706, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Minnesota North
    (26791, -101.012034, 46.219155, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Minnesota Central
    (26792, -101.955360, 44.733026, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Minnesota South
    (26793, -101.453125, 42.749738, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Mississippi East
    (26794, -90.407385, 29.657320, 1e-3, 0.0, 0.0, 8.77747653153e-07),
    # NAD27 - NAD27 / Mississippi West
    (26795, -91.920659, 30.490336, 1e-3, 0.0, 0.0, 9.69594644483e-07),
    # NAD27 - NAD27 / Missouri East
    (26796, -92.186465, 35.821500, 1e-3, 0.0, 0.0, 1.67538151369e-06),
    # NAD27 - NAD27 / Missouri Central
    (26797, -94.186465, 35.821500, 1e-3, 0.0, 0.0, 1.67538151369e-06),
    # NAD27 - NAD27 / Missouri West
    (26798, -96.193560, 36.154688, 1e-3, 0.0, 0.0, 1.71869974268e-06),
    # NAD27 - NAD27 / California zone VII
    (26799, -130.468214, 22.090014, 2e-3, 0.0, 0.0, 5.32805279363e-07),
    # NAD27 Michigan - NAD Michigan / Michigan East (deprecated)
    (26801, -85.491444, 41.485522, 1e-3, 0.0, 0.0, 2.7736103068e-06),
    # NAD27 Michigan - NAD Michigan / Michigan Old Central (deprecated)
    (26802, -87.574839, 41.485521, 1e-3, 0.0, 0.0, 2.76767768492e-06),
    # NAD27 Michigan - NAD Michigan / Michigan West (deprecated)
    (26803, -90.574839, 41.485521, 1e-3, 0.0, 0.0, 2.76767768492e-06),
    # NAD27 Michigan - NAD Michigan / Michigan North (deprecated)
    (26811, -94.676019, 44.517614, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 Michigan - NAD Michigan / Michigan Central (deprecated)
    (26812, -91.823849, 43.063216, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 Michigan - NAD Michigan / Michigan South (deprecated)
    (26813, -91.614170, 41.262583, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Maine East (ftUS) (deprecated)
    (26814, -72.216114, 43.606225, 1e-3, 0.0, 0.0, 4.76777649198e-05),
    # NAD83 - NAD83 / Maine West (ftUS) (deprecated)
    (26815, -81.077225, 42.310713, 1e-3, 0.0, 0.0, 0.102337038947),
    # NAD83 - NAD83 / Minnesota North (ftUS) (deprecated)
    (26819, -103.287887, 45.126033, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Minnesota Central (ftUS) (deprecated)
    (26820, -104.182085, 43.648811, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Minnesota South (ftUS) (deprecated)
    (26821, -103.618869, 41.676372, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Nebraska (ftUS) (deprecated)
    (26822, -105.831722, 39.681418, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / West Virginia North (ftUS) (deprecated)
    (26823, -101.605849, 36.308365, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / West Virginia South (ftUS) (deprecated)
    (26824, -102.706272, 34.914495, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Maine East (ftUS) (deprecated)
    (26825, -72.216114, 43.606225, 1e-3, 0.0, 0.0, 4.76777649198e-05),
    # NAD83(HARN) - NAD83(HARN) / Maine West (ftUS) (deprecated)
    (26826, -81.077225, 42.310713, 1e-3, 0.0, 0.0, 0.102337038947),
    # NAD83(HARN) - NAD83(HARN) / Minnesota North (ftUS) (deprecated)
    (26830, -103.287887, 45.126033, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Minnesota Central (ftUS) (deprecated)
    (26831, -104.182085, 43.648811, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Minnesota South (ftUS) (deprecated)
    (26832, -103.618869, 41.676372, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Nebraska (ftUS) (deprecated)
    (26833, -105.831722, 39.681418, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / West Virginia North (ftUS) (deprecated)
    (26834, -101.605849, 36.308365, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / West Virginia South (ftUS) (deprecated)
    (26835, -102.706272, 34.914495, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maine East (ftUS) (deprecated)
    (26836, -72.216114, 43.606225, 1e-3, 0.0, 0.0, 4.76777649198e-05),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maine West (ftUS) (deprecated)
    (26837, -81.077225, 42.310713, 1e-3, 0.0, 0.0, 0.102337038947),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Minnesota North (ftUS) (deprecated)
    (26841, -103.287887, 45.126033, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Minnesota Central (ftUS) (deprecated)
    (26842, -104.182085, 43.648811, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Minnesota South (ftUS) (deprecated)
    (26843, -103.618869, 41.676372, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Nebraska (ftUS) (deprecated)
    (26844, -105.831722, 39.681418, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / West Virginia N (ftUS) (deprecated)
    (26845, -101.605849, 36.308365, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / West Virginia S (ftUS) (deprecated)
    (26846, -102.706272, 34.914495, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Maine East (ftUS)
    (26847, -72.216114, 43.606225, 1e-3, 0.0, 0.0, 0.000156422227499),
    # NAD83 - NAD83 / Maine West (ftUS)
    (26848, -81.077225, 42.310713, 1e-3, 0.0, 0.0, 0.335750768611),
    # NAD83 - NAD83 / Minnesota North (ftUS)
    (26849, -103.287887, 45.126033, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Minnesota Central (ftUS)
    (26850, -104.182085, 43.648811, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Minnesota South (ftUS)
    (26851, -103.618869, 41.676372, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Nebraska (ftUS)
    (26852, -105.831722, 39.681418, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / West Virginia North (ftUS)
    (26853, -86.363854, 38.293447, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / West Virginia South (ftUS)
    (26854, -87.727921, 36.803713, 1e-3, 0.0, 0.0, 1.63071231427e-07),
    # NAD83(HARN) - NAD83(HARN) / Maine East (ftUS)
    (26855, -72.216114, 43.606225, 1e-3, 0.0, 0.0, 0.000156422227499),
    # NAD83(HARN) - NAD83(HARN) / Maine West (ftUS)
    (26856, -81.077225, 42.310713, 1e-3, 0.0, 0.0, 0.335750768611),
    # NAD83(HARN) - NAD83(HARN) / Minnesota North (ftUS)
    (26857, -103.287887, 45.126033, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Minnesota Central (ftUS)
    (26858, -104.182085, 43.648811, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Minnesota South (ftUS)
    (26859, -103.618869, 41.676372, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / Nebraska (ftUS)
    (26860, -105.831722, 39.681418, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / West Virginia North (ftUS)
    (26861, -86.363854, 38.293447, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(HARN) - NAD83(HARN) / West Virginia South (ftUS)
    (26862, -87.727921, 36.803713, 1e-3, 0.0, 0.0, 1.63071231427e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maine East (ftUS)
    (26863, -72.216114, 43.606225, 1e-3, 0.0, 0.0, 0.000156422227499),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Maine West (ftUS)
    (26864, -81.077225, 42.310713, 1e-3, 0.0, 0.0, 0.335750768611),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Minnesota North (ftUS)
    (26865, -103.287887, 45.126033, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Minnesota Central (ftUS)
    (26866, -104.182085, 43.648811, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Minnesota South (ftUS)
    (26867, -103.618869, 41.676372, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / Nebraska (ftUS)
    (26868, -105.831722, 39.681418, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / West Virginia North (ftUS)
    (26869, -86.363854, 38.293447, 1e-3, 0.0, 0.0, 1e-07),
    # NAD83(NSRS2007) - NAD83(NSRS2007) / West Virginia South (ftUS)
    (26870, -87.727921, 36.803713, 1e-3, 0.0, 0.0, 1.63071231427e-07),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 11
    (26891, -85.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 12
    (26892, -83.737290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 13
    (26893, -86.737290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 14
    (26894, -89.737290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 15
    (26895, -92.737290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 16
    (26896, -95.737290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 17
    (26897, -98.737290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 1
    (26898, -55.737290, 0.0, 1e-4, 0.0, 0.0, 1.07595697045e-05),
    # NAD83(CSRS) - NAD83(CSRS) / MTM zone 2
    (26899, -58.737290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / UTM zone 1N
    (26901, 178.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167734193383),
    # NAD83 - NAD83 / UTM zone 2N
    (26902, -175.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 3N
    (26903, -169.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 4N
    (26904, -163.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 5N
    (26905, -157.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167734193383),
    # NAD83 - NAD83 / UTM zone 6N
    (26906, -151.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 7N
    (26907, -145.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 8N
    (26908, -139.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 9N
    (26909, -133.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 10N
    (26910, -127.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 11N
    (26911, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 12N
    (26912, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83 - NAD83 / UTM zone 13N
    (26913, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83 - NAD83 / UTM zone 14N
    (26914, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83 - NAD83 / UTM zone 15N
    (26915, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83 - NAD83 / UTM zone 16N
    (26916, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # NAD83 - NAD83 / UTM zone 17N
    (26917, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83 - NAD83 / UTM zone 18N
    (26918, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83 - NAD83 / UTM zone 19N
    (26919, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83 - NAD83 / UTM zone 20N
    (26920, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83 - NAD83 / UTM zone 21N
    (26921, -61.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # NAD83 - NAD83 / UTM zone 22N
    (26922, -55.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # NAD83 - NAD83 / UTM zone 23N
    (26923, -49.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # NAD83 - NAD83 / Alabama East
    (26929, -87.916194, 30.483360, 1e-4, 0.0, 0.0, 1.33309797832e-06),
    # NAD83 - NAD83 / Alabama West
    (26930, -93.703674, 29.853763, 1e-4, 0.0, 0.0, 0.00067773398073),
    # NAD83 - NAD83 / Alaska zone 1
    (26931, -145.375377, 51.236569, 1e-4, 0.0, 0.0, 1.76858156919e-06),
    # NAD83 - NAD83 / Alaska zone 2
    (26932, -149.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726326207989),
    # NAD83 - NAD83 / Alaska zone 3
    (26933, -153.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83 - NAD83 / Alaska zone 4
    (26934, -157.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83 - NAD83 / Alaska zone 5
    (26935, -161.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83 - NAD83 / Alaska zone 6
    (26936, -165.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83 - NAD83 / Alaska zone 7
    (26937, -169.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726326207989),
    # NAD83 - NAD83 / Alaska zone 8
    (26938, -173.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726326207989),
    # NAD83 - NAD83 / Alaska zone 9
    (26939, -177.588605, 53.759438, 1e-4, 0.0, 0.0, 0.00726325936965),
    # NAD83 - NAD83 / Alaska zone 10
    (26940, 169.941515, 50.118842, 1e-4, 0.0, 0.0, 2.16796352387e-06),
    # NAD83 - NAD83 / California zone 1
    (26941, -143.321156, 32.649532, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / California zone 2
    (26942, -142.954019, 31.095993, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / California zone 3
    (26943, -141.218751, 30.009900, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / California zone 4
    (26944, -139.486472, 28.915572, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / California zone 5
    (26945, -138.152186, 27.196010, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / California zone 6
    (26946, -136.178771, 25.945490, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Arizona East
    (26948, -112.400208, 30.980684, 1e-4, 0.0, 0.0, 2.03809133589e-06),
    # NAD83 - NAD83 / Arizona Central
    (26949, -114.150208, 30.980684, 1e-4, 0.0, 0.0, 2.03625043994e-06),
    # NAD83 - NAD83 / Arizona West
    (26950, -115.983467, 30.980685, 1e-4, 0.0, 0.0, 2.03845919723e-06),
    # NAD83 - NAD83 / Arkansas North
    (26951, -96.343203, 34.253806, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Arkansas South
    (26952, -96.090762, 28.992599, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Colorado North
    (26953, -115.653598, 36.118325, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Colorado Central
    (26954, -115.464063, 34.638233, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Colorado South
    (26955, -115.330280, 33.489028, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Connecticut
    (26956, -76.287472, 39.405061, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Delaware
    (26957, -77.692913, 37.977968, 1e-4, 0.0, 0.0, 2.82291068248e-06),
    # NAD83 - NAD83 / Florida East
    (26958, -82.970337, 24.320543, 1e-4, 0.0, 0.0, 4.93879810895e-07),
    # NAD83 - NAD83 / Florida West
    (26959, -83.970337, 24.320543, 1e-4, 0.0, 0.0, 4.93879810895e-07),
    # NAD83 - NAD83 / Florida North
    (26960, -90.650782, 28.853971, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Hawaii zone 1
    (26961, -160.238151, 18.773126, 1e-4, 0.0, 0.0, 2.68067027011e-05),
    # NAD83 - NAD83 / Hawaii zone 2
    (26962, -161.448688, 20.267936, 1e-4, 0.0, 0.0, 5.14780165235e-05),
    # NAD83 - NAD83 / Hawaii zone 3
    (26963, -162.808055, 21.098347, 1e-4, 0.0, 0.0, 6.66313252259e-05),
    # NAD83 - NAD83 / Hawaii zone 4
    (26964, -164.329911, 21.762651, 1e-4, 0.0, 0.0, 7.90273515641e-05),
    # NAD83 - NAD83 / Hawaii zone 5
    (26965, -164.990986, 21.596578, 1e-4, 0.0, 0.0, 7.59037093138e-05),
    # NAD83 - NAD83 / Georgia East
    (26966, -84.239141, 29.983687, 1e-4, 0.0, 0.0, 1.25848615201e-06),
    # NAD83 - NAD83 / Georgia West
    (26967, -91.398144, 29.801257, 1e-4, 0.0, 0.0, 0.00142275930678),
    # NAD83 - NAD83 / Idaho East
    (26968, -114.567265, 41.641591, 1e-4, 0.0, 0.0, 3.94830305492e-06),
    # NAD83 - NAD83 / Idaho Central
    (26969, -119.988244, 41.510438, 1e-4, 0.0, 0.0, 0.000560564359692),
    # NAD83 - NAD83 / Idaho West
    (26970, -125.292410, 41.269025, 1e-4, 0.0, 0.0, 0.0284014806996),
    # NAD83 - NAD83 / Illinois East
    (26971, -91.686566, 36.619446, 1e-4, 0.0, 0.0, 2.51901485408e-05),
    # NAD83 - NAD83 / Illinois West
    (26972, -97.964340, 36.410947, 1e-4, 0.0, 0.0, 0.00255660697676),
    # NAD83 - NAD83 / Indiana East
    (26973, -86.765357, 35.241995, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Indiana West
    (26974, -96.907562, 34.847248, 1e-4, 0.0, 0.0, 0.01940148008),
    # NAD83 - NAD83 / Iowa North
    (26975, -109.055583, 31.312141, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Iowa South
    (26976, -99.345436, 39.848662, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Kansas North
    (26977, -102.570641, 38.242381, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Kansas South
    (26978, -102.765768, 32.984182, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Kentucky North (deprecated)
    (26979, -89.897363, 37.363376, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Kentucky South
    (26980, -91.003100, 31.708902, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Louisiana North
    (26981, -102.882630, 30.067691, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Louisiana South
    (26982, -101.517796, 28.098894, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Maine East
    (26983, -72.216114, 43.606225, 1e-4, 0.0, 0.0, 4.76777649198e-05),
    # NAD83 - NAD83 / Maine West
    (26984, -81.077225, 42.310713, 1e-4, 0.0, 0.0, 0.102337038947),
    # NAD83 - NAD83 / Maryland
    (26985, -81.529189, 37.577262, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Massachusetts Mainland
    (26986, -73.651391, 34.244387, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Massachusetts Island
    (26987, -76.433406, 40.845825, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Michigan North
    (26988, -158.790109, 11.663980, 1e-4, 0.0, 0.0, 2.29428967657e-07),
    # NAD83 - NAD83 / Michigan Central
    (26989, -144.322231, 22.874000, 1e-4, 0.0, 0.0, 2.08924502698e-07),
    # NAD83 - NAD83 / Michigan South
    (26990, -127.914179, 32.026364, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Minnesota North
    (26991, -103.287887, 45.126033, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Minnesota Central
    (26992, -104.182085, 43.648811, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Minnesota South
    (26993, -103.618869, 41.676372, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Mississippi East
    (26994, -91.925475, 29.464053, 1e-4, 0.0, 0.0, 1.2232144699e-05),
    # NAD83 - NAD83 / Mississippi West
    (26995, -97.529032, 29.305235, 1e-4, 0.0, 0.0, 0.00138477058454),
    # NAD83 - NAD83 / Missouri East
    (26996, -93.265663, 35.801506, 1e-4, 0.0, 0.0, 8.21736460543e-06),
    # NAD83 - NAD83 / Missouri Central
    (26997, -98.022683, 35.706341, 1e-4, 0.0, 0.0, 0.000399243352018),
    # NAD83 - NAD83 / Missouri West
    (26998, -103.890580, 35.797519, 1e-4, 0.0, 0.0, 0.0140642820106),
    # Nahrwan 1967 - Nahrwan 1967 / UTM zone 37N (deprecated)
    (27037, 34.511448, 0.003446, 1e-4, 0.0, 0.0, 0.000172125786776),
    # Nahrwan 1967 - Nahrwan 1967 / UTM zone 38N (deprecated)
    (27038, 40.511723, 0.003446, 1e-4, 0.0, 0.0, 0.000172127882337),
    # Nahrwan 1967 - Nahrwan 1967 / UTM zone 39N
    (27039, 46.511994, 0.003446, 1e-4, 0.0, 0.0, 0.000172126834514),
    # Nahrwan 1967 - Nahrwan 1967 / UTM zone 40N
    (27040, 52.512257, 0.003446, 1e-4, 0.0, 0.0, 0.000172126834514),
    # Naparima 1972 - Naparima 1972 / UTM zone 20N
    (27120, -67.487361, 0.001492, 1e-4, 0.0, 0.0, 0.000168838771103),
    # NZGD49 - NZGD49 / Mount Eden Circuit
    (27205, 171.077794, -43.123496, 1e-4, 0.0, 0.0, 0.000184122647624),
    # NZGD49 - NZGD49 / Bay of Plenty Circuit
    (27206, 172.726035, -44.001483, 1e-4, 0.0, 0.0, 0.000179006281542),
    # NZGD49 - NZGD49 / Poverty Bay Circuit
    (27207, 174.090106, -44.862154, 1e-4, 0.0, 0.0, 0.000174904736923),
    # NZGD49 - NZGD49 / Hawkes Bay Circuit
    (27208, 172.808976, -45.884991, 1e-4, 0.0, 0.0, 0.000174932501977),
    # NZGD49 - NZGD49 / Taranaki Circuit
    (27209, 170.398458, -45.371524, 1e-4, 0.0, 0.0, 0.000179836177267),
    # NZGD49 - NZGD49 / Tuhirangi Circuit
    (27210, 171.784869, -45.746991, 1e-4, 0.0, 0.0, 0.000176824541995),
    # NZGD49 - NZGD49 / Wanganui Circuit
    (27211, 171.581819, -46.474016, 1e-4, 0.0, 0.0, 0.000176094530616),
    # NZGD49 - NZGD49 / Wairarapa Circuit
    (27212, 171.691343, -47.155263, 1e-4, 0.0, 0.0, 0.000175202294486),
    # NZGD49 - NZGD49 / Wellington Circuit
    (27213, 170.792480, -47.529745, 1e-4, 0.0, 0.0, 0.000176458444912),
    # NZGD49 - NZGD49 / Collingwood Circuit
    (27214, 168.731526, -46.945207, 1e-4, 0.0, 0.0, 0.000180618750164),
    # NZGD49 - NZGD49 / Nelson Circuit
    (27215, 169.317176, -47.503059, 1e-4, 0.0, 0.0, 0.000179112103069),
    # NZGD49 - NZGD49 / Karamea Circuit
    (27216, 168.125711, -47.518369, 1e-4, 0.0, 0.0, 0.000181336712558),
    # NZGD49 - NZGD49 / Buller Circuit
    (27217, 167.558020, -48.037431, 1e-4, 0.0, 0.0, 0.000182221265277),
    # NZGD49 - NZGD49 / Grey Circuit
    (27218, 167.485308, -48.558464, 1e-4, 0.0, 0.0, 0.000182336691068),
    # NZGD49 - NZGD49 / Amuri Circuit
    (27219, 168.916990, -48.912612, 1e-4, 0.0, 0.0, 0.000179682247108),
    # NZGD49 - NZGD49 / Marlborough Circuit
    (27220, 169.799413, -47.772059, 1e-4, 0.0, 0.0, 0.000178089685505),
    # NZGD49 - NZGD49 / Hokitika Circuit
    (27221, 166.870652, -49.109096, 1e-4, 0.0, 0.0, 0.000183744501555),
    # NZGD49 - NZGD49 / Okarito Circuit
    (27222, 166.133010, -49.332083, 1e-4, 0.0, 0.0, 0.000185384385986),
    # NZGD49 - NZGD49 / Jacksons Bay Circuit
    (27223, 164.404126, -50.196540, 1e-4, 0.0, 0.0, 0.000189937505638),
    # NZGD49 - NZGD49 / Mount Pleasant Circuit
    (27224, 168.558666, -49.810832, 1e-4, 0.0, 0.0, 0.000180995237315),
    # NZGD49 - NZGD49 / Gawler Circuit
    (27225, 167.178590, -49.968314, 1e-4, 0.0, 0.0, 0.000183853553608),
    # NZGD49 - NZGD49 / Timaru Circuit
    (27226, 166.817513, -50.619365, 1e-4, 0.0, 0.0, 0.000185495708138),
    # NZGD49 - NZGD49 / Lindis Peak Circuit
    (27227, 165.197822, -50.951138, 1e-4, 0.0, 0.0, 0.000189459737157),
    # NZGD49 - NZGD49 / Mount Nicholas Circuit
    (27228, 164.091911, -51.347236, 1e-4, 0.0, 0.0, 0.000192680046894),
    # NZGD49 - NZGD49 / Mount York Circuit
    (27229, 163.391310, -51.776376, 1e-4, 0.0, 0.0, 0.000195270142285),
    # NZGD49 - NZGD49 / Observation Point Circuit
    (27230, 166.256693, -52.027861, 1e-4, 0.0, 0.0, 0.000190636696061),
    # NZGD49 - NZGD49 / North Taieri Circuit
    (27231, 165.906066, -52.073242, 1e-4, 0.0, 0.0, 0.00019061905914),
    # NZGD49 - NZGD49 / Bluff Circuit
    (27232, 163.892835, -52.808519, 1e-4, 0.0, 0.0, 0.000198742171051),
    # NZGD49 - NZGD49 / UTM zone 58S
    (27258, 8.586443, -89.999434, 1e-4, 0.0, 0.0, 3),
    # NZGD49 - NZGD49 / UTM zone 59S
    (27259, 8.586443, -89.999434, 1e-4, 0.0, 0.0, 3),
    # NZGD49 - NZGD49 / UTM zone 60S
    (27260, 8.586443, -89.999434, 1e-4, 0.0, 0.0, 3),
    # NZGD49 - NZGD49 / North Island Grid
    (27291, 172.176649, -42.243758, 1e-4, 0.0, 0.0, 0.000183536158062),
    # NZGD49 - NZGD49 / South Island Grid
    (27292, 165.378439, -47.948170, 1e-4, 0.0, 0.0, 0.0013442113421),
    # NGO 1948 (Oslo) - NGO 1948 (Oslo) / NGO zone I
    (27391, 6.052015, 58.000826, 1e-4, 0.0, 0.0, 0.0268471625473),
    # NGO 1948 (Oslo) - NGO 1948 (Oslo) / NGO zone II
    (27392, 8.385167, 58.000897, 1e-4, 0.0, 0.0, 0.0269763588743),
    # NGO 1948 (Oslo) - NGO 1948 (Oslo) / NGO zone III
    (27393, 10.718324, 58.000970, 1e-4, 0.0, 0.0, 0.0271170566471),
    # NGO 1948 (Oslo) - NGO 1948 (Oslo) / NGO zone IV
    (27394, 13.218139, 58.001053, 1e-4, 0.0, 0.0, 0.0272811783457),
    # NGO 1948 (Oslo) - NGO 1948 (Oslo) / NGO zone V
    (27395, 16.884545, 58.001179, 1e-4, 0.0, 0.0, 0.0275476151682),
    # NGO 1948 (Oslo) - NGO 1948 (Oslo) / NGO zone VI
    (27396, 20.884276, 58.001325, 1e-4, 0.0, 0.0, 0.0278730627625),
    # NGO 1948 (Oslo) - NGO 1948 (Oslo) / NGO zone VII
    (27397, 24.884024, 58.001478, 1e-4, 0.0, 0.0, 0.0282329943568),
    # NGO 1948 (Oslo) - NGO 1948 (Oslo) / NGO zone VIII
    (27398, 29.050447, 58.001644, 1e-4, 0.0, 0.0, 0.0286404896626),
    # Datum 73 - Datum 73 / UTM zone 29N
    (27429, -13.488073, 0.000331, 1e-4, 0.0, 0.0, 0.000168840604623),
    # Datum 73 - Datum 73 / Modified Portuguese Grid (deprecated)
    (27492, -8.133108, 39.668256, 1e-4, 0.0, 0.0, 1e-07),
    # Datum 73 - Datum 73 / Modified Portuguese Grid
    (27493, -8.133108, 39.668256, 1e-4, 0.0, 0.0, 1e-07),
    # ATF (Paris) - ATF (Paris) / Nord de Guerre
    (27500, 1.205637, 46.606096, 1e-4, 0.0, 0.0, 2.96090729535e-06),
    # NTF (Paris) - NTF (Paris) / Lambert Nord France
    (27561, -5.624907, 47.416193, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / Lambert Centre France
    (27562, -5.246775, 44.740063, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / Lambert Sud France
    (27563, -4.918067, 42.061768, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / Lambert Corse
    (27564, 2.333840, 40.491671, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / Lambert zone I
    (27571, -4.435933, 38.520074, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / Lambert zone II
    (27572, -3.404561, 27.140973, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / Lambert zone III
    (27573, -2.701997, 16.123092, 1e-4, 0.0, 0.0, 2.02737282962e-07),
    # NTF (Paris) - NTF (Paris) / Lambert zone IV
    (27574, 2.334969, 6.587316, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / France I (deprecated)
    (27581, -4.435933, 38.520074, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / France II (deprecated)
    (27582, -3.404561, 27.140973, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / France III (deprecated)
    (27583, -2.701997, 16.123092, 1e-4, 0.0, 0.0, 2.02737282962e-07),
    # NTF (Paris) - NTF (Paris) / France IV (deprecated)
    (27584, 2.334969, 6.587316, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / Nord France (deprecated)
    (27591, -5.624907, 47.416193, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / Centre France (deprecated)
    (27592, -5.246775, 44.740063, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / Sud France (deprecated)
    (27593, -4.918067, 42.061768, 1e-4, 0.0, 0.0, 1e-07),
    # NTF (Paris) - NTF (Paris) / Corse (deprecated)
    (27594, 2.333840, 40.491671, 1e-4, 0.0, 0.0, 1e-07),
    # OSGB 1936 - OSGB 1936 / British National Grid
    (27700, -7.557160, 49.766807, 1e-4, 0.0, 0.0, 0.000684377395373),
    # Palestine 1923 - Palestine 1923 / Palestine Grid
    (28191, 33.437802, 30.578180, 1e-4, 0.0, 0.0, 0.0264388597134),
    # Palestine 1923 - Palestine 1923 / Palestine Belt
    (28192, 33.568589, 21.556259, 1e-4, 0.0, 0.0, 0.0373639346362),
    # Palestine 1923 - Palestine 1923 / Israeli CS Grid
    (28193, 33.568393, 21.556257, 1e-4, 0.0, 0.0, 0.0356336460536),
    # Pointe Noire - Pointe Noire / UTM zone 32S
    (28232, 160.986326, -89.998598, 1e-4, 0.0, 0.0, 753200),
    # GDA94 - GDA94 / MGA zone 48
    (28348, 105.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 49
    (28349, 111.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 50
    (28350, 117.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 51
    (28351, 123.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 52
    (28352, 129.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 53
    (28353, 135.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 54
    (28354, 141.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 55
    (28355, 147.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 56
    (28356, 153.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 57
    (28357, 159.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # GDA94 - GDA94 / MGA zone 58
    (28358, 165.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 2 (deprecated)
    (28402, -12.900840, -0.000827, 1e-4, 0.0, 0.0, 19),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 3 (deprecated)
    (28403, -14.962292, -0.000826, 1e-4, 0.0, 0.0, 325),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 4
    (28404, -16.408634, -0.000826, 1e-4, 0.0, 0.0, 2788),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 5
    (28405, -17.144849, -0.000825, 1e-4, 0.0, 0.0, 15649),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 6
    (28406, -17.069325, -0.000825, 1e-4, 0.0, 0.0, 66222),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 7
    (28407, -15.997783, -0.000826, 1e-4, 0.0, 0.0, 228895),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 8
    (28408, -13.542451, -0.000827, 1e-4, 0.0, 0.0, 675426),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 9
    (28409, -8.938328, -0.000828, 1e-4, 0.0, 0.0, 1736227),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 10
    (28410, -0.808414, -0.000829, 1e-4, 0.0, 0.0, 3900661),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 11
    (28411, 13.140266, -0.000827, 1e-4, 0.0, 0.0, 7620250),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 12
    (28412, 36.497984, -0.000810, 1e-4, 0.0, 0.0, 13003845.6946),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 13
    (28413, 74.619114, -0.000758, 1e-4, 0.0, 0.0, 20186451.1939),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 14
    (28414, 135.182171, -0.000662, 1e-4, 0.0, 0.0, 32540211.601),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 15
    (28415, -131.150014, -0.000667, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 16
    (28416, 10.043813, -0.000828, 1e-4, 0.0, 0.0, 1897470),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 17
    (28417, -142.158267, -0.000654, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 18
    (28418, 156.971573, -0.000642, 1e-4, 0.0, 0.0, 37933969.7858),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 19
    (28419, -141.014589, -0.000656, 1e-4, 0.0, 0.0, 4),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 20
    (28420, 83.538448, -0.000743, 1e-4, 0.0, 0.0, 24812506.3052),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 21
    (28421, 159.842578, -0.000640, 1e-4, 0.0, 0.0, 38878499.3144),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 22
    (28422, 148.253312, -0.000648, 1e-4, 0.0, 0.0, 37027455.3706),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 23
    (28423, 122.053573, -0.000680, 1e-4, 0.0, 0.0, 33069293.0705),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 24
    (28424, 169.391885, -0.000635, 1e-4, 0.0, 0.0, 41698979.5418),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 25
    (28425, 35.403339, -0.000811, 1e-4, 0.0, 0.0, 0.927051140613),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 26
    (28426, -155.454263, -0.000643, 1e-4, 0.0, 0.0, 49820287.877),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 27
    (28427, 103.088886, -0.000709, 1e-4, 0.0, 0.0, 29965077.926),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 28
    (28428, -98.130671, -0.000718, 1e-4, 0.0, 0.0, 3),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 29
    (28429, 159.211650, -0.000640, 1e-4, 0.0, 0.0, 42267195.8386),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 30
    (28430, 24.096823, -0.000821, 1e-4, 0.0, 0.0, 0.631054175367),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 31
    (28431, 119.571786, -0.000683, 1e-4, 0.0, 0.0, 33569247.8908),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger zone 32
    (28432, 26.360989, -0.000819, 1e-4, 0.0, 0.0, 0.690328692228),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 2N (deprecated)
    (28462, 4.512070, -0.000829, 1e-4, 0.0, 0.0, 0.000348863587362),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 3N (deprecated)
    (28463, 10.512065, -0.000828, 1e-4, 0.0, 0.0, 0.000352046177593),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 4N (deprecated)
    (28464, 16.512074, -0.000825, 1e-4, 0.0, 0.0, 0.0003547533341),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 5N (deprecated)
    (28465, 22.512097, -0.000822, 1e-4, 0.0, 0.0, 0.000357078236813),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 6N (deprecated)
    (28466, 28.512134, -0.000818, 1e-4, 0.0, 0.0, 0.000359142031643),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 7N (deprecated)
    (28467, 34.512184, -0.000812, 1e-4, 0.0, 0.0, 0.000361041860741),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 8N (deprecated)
    (28468, 40.512247, -0.000806, 1e-4, 0.0, 0.0, 0.000362892720337),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 9N (deprecated)
    (28469, 46.512322, -0.000799, 1e-4, 0.0, 0.0, 0.000364781442513),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 10N (deprecated)
    (28470, 52.512409, -0.000791, 1e-4, 0.0, 0.0, 0.000366786985697),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 11N (deprecated)
    (28471, 58.512505, -0.000783, 1e-4, 0.0, 0.0, 0.000368971420167),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 12N (deprecated)
    (28472, 64.512611, -0.000774, 1e-4, 0.0, 0.0, 0.000371366311652),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 13N (deprecated)
    (28473, 70.512726, -0.000764, 1e-4, 0.0, 0.0, 0.000373968467309),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 14N (deprecated)
    (28474, 76.512847, -0.000754, 1e-4, 0.0, 0.0, 0.000376742069684),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 15N (deprecated)
    (28475, 82.512973, -0.000744, 1e-4, 0.0, 0.0, 0.000379647888205),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 16N (deprecated)
    (28476, 88.513104, -0.000734, 1e-4, 0.0, 0.0, 0.000382578025962),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 17N (deprecated)
    (28477, 94.513237, -0.000724, 1e-4, 0.0, 0.0, 0.000385434473965),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 18N (deprecated)
    (28478, 100.513372, -0.000714, 1e-4, 0.0, 0.0, 0.000388083772623),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 19N (deprecated)
    (28479, 106.513506, -0.000704, 1e-4, 0.0, 0.0, 0.000390382135813),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 20N (deprecated)
    (28480, 112.513639, -0.000694, 1e-4, 0.0, 0.0, 0.000392181631197),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 21N (deprecated)
    (28481, 118.513768, -0.000685, 1e-4, 0.0, 0.0, 0.000393307900105),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 22N (deprecated)
    (28482, 124.513893, -0.000676, 1e-4, 0.0, 0.0, 0.000393654881024),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 23N (deprecated)
    (28483, 130.514013, -0.000668, 1e-4, 0.0, 0.0, 0.000393044411281),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 24N (deprecated)
    (28484, 136.514124, -0.000661, 1e-4, 0.0, 0.0, 0.000391412736055),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 25N (deprecated)
    (28485, 142.514228, -0.000654, 1e-4, 0.0, 0.0, 0.000388648486567),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 26N (deprecated)
    (28486, 148.514321, -0.000648, 1e-4, 0.0, 0.0, 0.000384712435489),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 27N (deprecated)
    (28487, 154.514405, -0.000643, 1e-4, 0.0, 0.0, 0.000379595055897),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 28N (deprecated)
    (28488, 160.514476, -0.000639, 1e-4, 0.0, 0.0, 0.000373326502891),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 29N (deprecated)
    (28489, 166.514535, -0.000636, 1e-4, 0.0, 0.0, 0.000365976858996),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 30N (deprecated)
    (28490, 172.514582, -0.000635, 1e-4, 0.0, 0.0, 0.000357641063587),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 31N (deprecated)
    (28491, 178.514615, -0.000634, 1e-4, 0.0, 0.0, 0.000348491218294),
    # Pulkovo 1942 - Pulkovo 1942 / Gauss-Kruger 32N (deprecated)
    (28492, -175.485366, -0.000634, 1e-4, 0.0, 0.0, 0.000348862190418),
    # Qatar 1974 - Qatar 1974 / Qatar National Grid
    (28600, 49.282976, 21.730437, 1e-4, 0.0, 0.0, 1.96843757294e-07),
    # Amersfoort - Amersfoort / RD Old
    (28991, 5.387204, 52.155172, 1e-4, 0.0, 0.0, 0.000678913596384),
    # Amersfoort - Amersfoort / RD New
    (28992, 3.313558, 47.974766, 1e-4, 0.0, 0.0, 0.000737458394724),
    # SAD69 - SAD69 / UTM zone 18N (deprecated)
    (29118, -79.489230, -0.000371, 1e-4, 0.0, 0.0, 0.000167736900039),
    # SAD69 - SAD69 / UTM zone 19N (deprecated)
    (29119, -73.489216, -0.000371, 1e-4, 0.0, 0.0, 0.00016773690005),
    # SAD69 - SAD69 / UTM zone 20N (deprecated)
    (29120, -67.489197, -0.000371, 1e-4, 0.0, 0.0, 0.000167741178302),
    # SAD69 - SAD69 / UTM zone 21N (deprecated)
    (29121, -61.489173, -0.000371, 1e-4, 0.0, 0.0, 0.000167735503077),
    # SAD69 - SAD69 / UTM zone 22N (deprecated)
    (29122, -55.489145, -0.000371, 1e-4, 0.0, 0.0, 0.000167733407579),
    # SAD69 - SAD69 / UTM zone 18N
    (29168, -79.489311, -0.000348, 1e-4, 0.0, 0.0, 0.000167733844158),
    # SAD69 - SAD69 / UTM zone 19N
    (29169, -73.489293, -0.000348, 1e-4, 0.0, 0.0, 0.000167733844148),
    # SAD69 - SAD69 / UTM zone 20N
    (29170, -67.489268, -0.000348, 1e-4, 0.0, 0.0, 0.000167733145656),
    # SAD69 - SAD69 / UTM zone 21N
    (29171, -61.489237, -0.000348, 1e-4, 0.0, 0.0, 0.000167732447164),
    # SAD69 - SAD69 / UTM zone 22N
    (29172, -55.489200, -0.000348, 1e-4, 0.0, 0.0, 0.000167737685853),
    # SAD69 - SAD69 / UTM zone 17S (deprecated)
    (29177, 178.994914, -89.999490, 1e-4, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 18S (deprecated)
    (29178, 178.994914, -89.999490, 1e-4, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 19S (deprecated)
    (29179, 178.994914, -89.999490, 1e-4, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 20S (deprecated)
    (29180, 178.994914, -89.999490, 1e-4, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 21S (deprecated)
    (29181, 178.994914, -89.999490, 1e-4, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 22S (deprecated)
    (29182, 178.994914, -89.999490, 1e-4, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 23S (deprecated)
    (29183, 178.994914, -89.999490, 1e-4, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 24S (deprecated)
    (29184, 178.994914, -89.999490, 1e-4, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 25S (deprecated)
    (29185, 178.994914, -89.999490, 1e-4, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 17S
    (29187, 176.260999, -89.999400, 3, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 18S
    (29188, 176.260999, -89.999400, 3, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 19S
    (29189, 176.260999, -89.999400, 3, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 20S
    (29190, 176.260999, -89.999400, 3, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 21S
    (29191, 176.260999, -89.999400, 3, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 22S
    (29192, 176.260999, -89.999400, 3, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 23S
    (29193, 176.260999, -89.999400, 3, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 24S
    (29194, 176.260999, -89.999400, 3, 0.0, 0.0, 753000),
    # SAD69 - SAD69 / UTM zone 25S
    (29195, 176.260999, -89.999400, 3, 0.0, 0.0, 753000),
    # Sapper Hill 1943 - Sapper Hill 1943 / UTM zone 20S
    (29220, 176.614617, -89.996816, 1e-4, 0.0, 0.0, 752569),
    # Sapper Hill 1943 - Sapper Hill 1943 / UTM zone 21S
    (29221, 176.614617, -89.996816, 1e-4, 0.0, 0.0, 752569),
    # Schwarzeck - Schwarzeck / UTM zone 33S
    (29333, 8.948743, -89.994417, 1e-4, 0.0, 0.0, 754513),
    # Schwarzeck - Schwarzeck / Lo22/11
    (29371, 10.999784, -22.000408, 1e-4, 0.0, 0.0, 1e-07),
    # Schwarzeck - Schwarzeck / Lo22/13
    (29373, 12.999573, -22.000412, 1e-4, 0.0, 0.0, 1e-07),
    # Schwarzeck - Schwarzeck / Lo22/15
    (29375, 14.999363, -22.000418, 1e-4, 0.0, 0.0, 1e-07),
    # Schwarzeck - Schwarzeck / Lo22/17
    (29377, 16.999154, -22.000427, 1e-4, 0.0, 0.0, 1e-07),
    # Schwarzeck - Schwarzeck / Lo22/19
    (29379, 18.998946, -22.000439, 1e-4, 0.0, 0.0, 1e-07),
    # Schwarzeck - Schwarzeck / Lo22/21
    (29381, 20.998739, -22.000453, 1e-4, 0.0, 0.0, 1e-07),
    # Schwarzeck - Schwarzeck / Lo22/23
    (29383, 22.998534, -22.000470, 1e-4, 0.0, 0.0, 1e-07),
    # Schwarzeck - Schwarzeck / Lo22/25
    (29385, 24.998330, -22.000489, 1e-4, 0.0, 0.0, 1e-07),
    # Sudan - Sudan / UTM zone 35N (deprecated)
    (29635, 22.511335, 0.0, 1e-4, 0.0, 0.0, 0.000172126572579),
    # Sudan - Sudan / UTM zone 36N (deprecated)
    (29636, 28.511335, 0.0, 1e-4, 0.0, 0.0, 0.000172126572579),
    # Tananarive (Paris) - Tananarive (Paris) / Laborde Grid (deprecated)
    (29700, 42.443549, -26.081208, 1e-4, 0.0, 0.0, 1e-07),
    # Tananarive (Paris) - Tananarive (Paris) / Laborde Grid approximation
    (29702, 42.443549, -26.081208, 1e-4, 0.0, 0.0, 1e-07),
    # Tananarive - Tananarive / UTM zone 38S
    (29738, -127.989542, -89.997251, 1e-4, 0.0, 0.0, 752569),
    # Tananarive - Tananarive / UTM zone 39S
    (29739, -127.989542, -89.997251, 1e-4, 0.0, 0.0, 752569),
    # Timbalai 1948 - Timbalai 1948 / UTM zone 49N
    (29849, 106.514807, -0.000434, 1e-4, 0.0, 0.0, 0.000165570527326),
    # Timbalai 1948 - Timbalai 1948 / UTM zone 50N
    (29850, 112.514001, -0.000434, 1e-4, 0.0, 0.0, 0.000165570527337),
    # Timbalai 1948 - Timbalai 1948 / RSO Borneo (ch)
    (29871, 109.689239, -0.000606, 1e-4, 0.0, 0.0, 1e-07),
    # Timbalai 1948 - Timbalai 1948 / RSO Borneo (ft)
    (29872, 109.689239, -0.000606, 1e-4, 0.0, 0.0, 1.94503298648e-07),
    # Timbalai 1948 - Timbalai 1948 / RSO Borneo (m)
    (29873, 109.689239, -0.000606, 1e-4, 0.0, 0.0, 1e-07),
    # TM65 - TM65 / Irish National Grid (deprecated)
    (29900, -10.863443, 51.218565, 1e-4, 0.0, 0.0, 0.000252986486885),
    # OSNI 1952 - OSNI 1952 / Irish National Grid
    (29901, -10.863443, 51.218559, 1e-4, 0.0, 0.0, 0.000252983518294),
    # TM65 - TM65 / Irish Grid
    (29902, -10.863443, 51.218565, 1e-4, 0.0, 0.0, 0.000252986486885),
    # TM75 - TM75 / Irish Grid
    (29903, -10.863443, 51.218565, 1e-4, 0.0, 0.0, 0.000252986486885),
    # Tokyo - Tokyo / Japan Plane Rectangular CS I
    (30161, 129.497756, 33.003313, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS II
    (30162, 130.997621, 33.003341, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS III
    (30163, 132.164093, 36.003018, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS IV
    (30164, 133.497399, 33.003391, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS V
    (30165, 134.330563, 36.003067, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS VI
    (30166, 135.997080, 36.003106, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS VII
    (30167, 137.163644, 36.003135, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS VIII
    (30168, 138.496862, 36.003169, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS IX
    (30169, 139.830081, 36.003205, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS X
    (30170, 140.829810, 40.002772, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS XI
    (30171, 140.246304, 44.002272, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS XII
    (30172, 142.246116, 44.002338, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS XIII
    (30173, 144.245933, 44.002407, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS XIV
    (30174, 141.996907, 26.004300, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS XV
    (30175, 127.498075, 26.004047, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS XVI
    (30176, 123.998379, 26.004005, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS XVII
    (30177, 130.997779, 26.004098, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS XVIII
    (30178, 135.997484, 20.004747, 1e-4, 0.0, 0.0, 1e-07),
    # Tokyo - Tokyo / Japan Plane Rectangular CS XIX
    (30179, 153.996086, 26.004592, 1e-4, 0.0, 0.0, 1e-07),
    # Trinidad 1903 - Trinidad 1903 / Trinidad Grid
    (30200, -62.121202, 9.853380, 1e-4, 0.0, 0.0, 0.000104564074822),
    # TC(1948) - TC(1948) / UTM zone 39N
    (30339, 46.511300, 0.0, 1e-4, 0.0, 0.0, 0.000167680409504),
    # TC(1948) - TC(1948) / UTM zone 40N
    (30340, 52.511300, 0.0, 1e-4, 0.0, 0.0, 0.000167681457242),
    # Voirol 1875 - Voirol 1875 / Nord Algerie (ancienne)
    (30491, -2.661459, 33.170841, 1e-4, 0.0, 0.0, 1e-07),
    # Voirol 1875 - Voirol 1875 / Sud Algerie (ancienne)
    (30492, -2.507774, 30.481252, 1e-4, 0.0, 0.0, 1e-07),
    # Voirol 1879 - Voirol 1879 / Nord Algerie (ancienne)
    (30493, -2.658777, 33.171708, 1e-4, 0.0, 0.0, 1e-07),
    # Voirol 1879 - Voirol 1879 / Sud Algerie (ancienne)
    (30494, -2.505171, 30.481955, 1e-4, 0.0, 0.0, 1e-07),
    # Nord Sahara 1959 - Nord Sahara 1959 / UTM zone 29N
    (30729, -13.489709, 0.002713, 1e-4, 0.0, 0.0, 0.00354353852738),
    # Nord Sahara 1959 - Nord Sahara 1959 / UTM zone 30N
    (30730, -7.489531, 0.002695, 1e-4, 0.0, 0.0, 0.00333124011053),
    # Nord Sahara 1959 - Nord Sahara 1959 / UTM zone 31N
    (30731, -1.489341, 0.002687, 1e-4, 0.0, 0.0, 0.00304556134766),
    # Nord Sahara 1959 - Nord Sahara 1959 / UTM zone 32N
    (30732, 4.510858, 0.002690, 1e-4, 0.0, 0.0, 0.00286098376449),
    # Nord Sahara 1959 - Nord Sahara 1959 / Nord Algerie
    (30791, -2.661024, 33.171047, 1e-4, 0.0, 0.0, 0.00295633697533),
    # Nord Sahara 1959 - Nord Sahara 1959 / Sud Algerie
    (30792, -2.507351, 30.481449, 1e-4, 0.0, 0.0, 0.00294203404337),
    # RT38 - RT38 2.5 gon W (deprecated)
    (30800, 2.455371, 0.0, 1e-4, 0.0, 0.0, 0.290561034693),
    # Yoff - Yoff / UTM zone 28N
    (31028, -19.487145, 0.000805, 2e-3, 0.0, 0.0, 0.000172128493453),
    # Zanderij - Zanderij / UTM zone 21N
    (31121, -61.490145, -0.003238, 1e-4, 0.0, 0.0, 0.00016883667567),
    # Zanderij - Zanderij / TM 54 NW
    (31154, -58.490034, -0.003238, 1e-4, 0.0, 0.0, 0.000168838771146),
    # Zanderij - Zanderij / Suriname Old TM
    (31170, -60.173430, -0.003238, 1e-4, 0.0, 0.0, 0.00016883344523),
    # Zanderij - Zanderij / Suriname TM
    (31171, -60.172086, -0.003238, 1e-4, 0.0, 0.0, 0.000168594560904),
    # MGI (Ferro) - MGI (Ferro) / Austria GK West Zone
    (31251, 10.329239, 45.139632, 1e-4, 0.0, 0.0, 1e-07),
    # MGI (Ferro) - MGI (Ferro) / Austria GK Central Zone
    (31252, 13.328822, 45.139745, 1e-4, 0.0, 0.0, 1e-07),
    # MGI (Ferro) - MGI (Ferro) / Austria GK East Zone
    (31253, 16.328418, 45.139869, 1e-4, 0.0, 0.0, 1e-07),
    # MGI - MGI / Austria GK West
    (31254, 10.333141, 45.139704, 1e-4, 0.0, 0.0, 0.000966961375667),
    # MGI - MGI / Austria GK Central
    (31255, 13.332747, 45.139792, 1e-4, 0.0, 0.0, 0.000473526054541),
    # MGI - MGI / Austria GK East
    (31256, 16.332358, 45.139890, 1e-4, 0.0, 0.0, 9.63067162215e-05),
    # MGI - MGI / Austria GK M28
    (31257, 8.426620, 45.123736, 1e-4, 0.0, 0.0, 0.00131107804191),
    # MGI - MGI / Austria GK M31
    (31258, 7.625832, 44.996791, 1e-4, 0.0, 0.0, 0.00183795753401),
    # MGI - MGI / Austria GK M34
    (31259, 6.862469, 44.745135, 1e-4, 0.0, 0.0, 0.03227351635),
    # MGI - MGI / 3-degree Gauss zone 5 (deprecated)
    (31265, -29.143959, 0.003136, 1e-4, 0.0, 0.0, 15664),
    # MGI - MGI / 3-degree Gauss zone 6 (deprecated)
    (31266, -32.068352, 0.003084, 1e-4, 0.0, 0.0, 66286),
    # MGI - MGI / 3-degree Gauss zone 7 (deprecated)
    (31267, -33.996295, 0.003051, 1e-4, 0.0, 0.0, 229123),
    # MGI - MGI / 3-degree Gauss zone 8 (deprecated)
    (31268, -34.539601, 0.003042, 1e-4, 0.0, 0.0, 676103),
    # MGI - MGI / Balkans zone 5 (deprecated)
    (31275, -29.147452, 0.003136, 1e-4, 0.0, 0.0, 15676),
    # MGI - MGI / Balkans zone 6 (deprecated)
    (31276, -32.071907, 0.003084, 1e-4, 0.0, 0.0, 66337),
    # MGI - MGI / Balkans zone 7 (deprecated)
    (31277, -33.999539, 0.003051, 1e-4, 0.0, 0.0, 229299),
    # MGI - MGI / Balkans zone 8 (deprecated)
    (31278, -33.999539, 0.003051, 1e-4, 0.0, 0.0, 229299),
    # MGI - MGI / Balkans zone 8 (deprecated)
    (31279, -34.541843, 0.003042, 1e-4, 0.0, 0.0, 676616),
    # MGI (Ferro) - MGI (Ferro) / Austria West Zone
    (31281, 10.330440, 0.004341, 1e-4, 0.0, 0.0, 1e-07),
    # MGI (Ferro) - MGI (Ferro) / Austria Central Zone
    (31282, 13.330146, 0.004341, 1e-4, 0.0, 0.0, 1e-07),
    # MGI (Ferro) - MGI (Ferro) / Austria East Zone
    (31283, 16.329860, 0.004341, 1e-4, 0.0, 0.0, 1e-07),
    # MGI - MGI / Austria M28
    (31284, 8.987290, 0.004013, 1e-4, 0.0, 0.0, 0.0135032761611),
    # MGI - MGI / Austria M31
    (31285, 9.295252, 0.004021, 1e-4, 0.0, 0.0, 0.0133906694641),
    # MGI - MGI / Austria M34
    (31286, 9.612173, 0.004029, 1e-4, 0.0, 0.0, 0.0140438756436),
    # MGI - MGI / Austria Lambert
    (31287, 8.368601, 43.786717, 1e-4, 0.0, 0.0, 0.00156686530681),
    # MGI (Ferro) - MGI (Ferro) / M28
    (31288, 8.983071, 0.004341, 1e-4, 0.0, 0.0, 2.72193574347e-07),
    # MGI (Ferro) - MGI (Ferro) / M31
    (31289, 9.291030, 0.004341, 1e-4, 0.0, 0.0, 9.09827650448e-05),
    # MGI (Ferro) - MGI (Ferro) / M34
    (31290, 9.607949, 0.004341, 1e-4, 0.0, 0.0, 0.00200209440672),
    # MGI (Ferro) - MGI (Ferro) / Austria West Zone (deprecated)
    (31291, 10.330440, 0.004341, 1e-4, 0.0, 0.0, 1e-07),
    # MGI (Ferro) - MGI (Ferro) / Austria Central Zone (deprecated)
    (31292, 13.330146, 0.004341, 1e-4, 0.0, 0.0, 1e-07),
    # MGI (Ferro) - MGI (Ferro) / Austria East Zone (deprecated)
    (31293, 16.329860, 0.004341, 1e-4, 0.0, 0.0, 1e-07),
    # MGI - MGI / M28 (deprecated)
    (31294, 8.987290, 0.004013, 1e-4, 0.0, 0.0, 0.0135032761611),
    # MGI - MGI / M31 (deprecated)
    (31295, 9.295252, 0.004021, 1e-4, 0.0, 0.0, 0.0133906694641),
    # MGI - MGI / M34 (deprecated)
    (31296, 9.612173, 0.004029, 1e-4, 0.0, 0.0, 0.0140438756436),
    # MGI - MGI / Austria Lambert (deprecated)
    (31297, 8.368601, 43.786717, 1e-4, 0.0, 0.0, 0.00156686530681),
    # Belge 1972 - Belge 1972 / Belge Lambert 72
    (31300, 2.296142, 49.293336, 1e-4, 0.0, 0.0, 0.00077149416029),
    # Belge 1972 - Belge 1972 / Belgian Lambert 72
    (31370, 2.306689, 49.293335, 1e-4, 0.0, 0.0, 0.000771557766711),
    # DHDN - DHDN / 3-degree Gauss zone 1 (deprecated)
    (31461, -10.351972, 0.003760, 1e-4, 0.0, 0.0, 0.292022259121),
    # DHDN - DHDN / 3-degree Gauss zone 2 (deprecated)
    (31462, -15.901231, 0.003755, 1e-4, 0.0, 0.0, 19),
    # DHDN - DHDN / 3-degree Gauss zone 3 (deprecated)
    (31463, -20.963134, 0.003750, 1e-4, 0.0, 0.0, 325),
    # DHDN - DHDN / 3-degree Gauss zone 4 (deprecated)
    (31464, -25.409789, 0.003747, 1e-4, 0.0, 0.0, 2790),
    # DHDN - DHDN / 3-degree Gauss zone 5 (deprecated)
    (31465, -29.146150, 0.003744, 1e-4, 0.0, 0.0, 15664),
    # DHDN - DHDN / 3-degree Gauss-Kruger zone 2
    (31466, -15.901231, 0.003755, 1e-4, 0.0, 0.0, 19),
    # DHDN - DHDN / 3-degree Gauss-Kruger zone 3
    (31467, -20.963134, 0.003750, 1e-4, 0.0, 0.0, 325),
    # DHDN - DHDN / 3-degree Gauss-Kruger zone 4
    (31468, -25.409789, 0.003747, 1e-4, 0.0, 0.0, 2790),
    # DHDN - DHDN / 3-degree Gauss-Kruger zone 5
    (31469, -29.146150, 0.003744, 1e-4, 0.0, 0.0, 15664),
    # Conakry 1905 - Conakry 1905 / UTM zone 28N
    (31528, -19.486540, -0.000081, 1e-4, 0.0, 0.0, 0.000172129104612),
    # Conakry 1905 - Conakry 1905 / UTM zone 29N
    (31529, -13.486450, -0.000081, 1e-4, 0.0, 0.0, 0.000172128231497),
    # Dealul Piscului 1930 - Dealul Piscului 1930 / Stereo 33
    (31600, 19.432308, 41.236016, 1e-4, 0.0, 0.0, 1e-07),
    # Dealul Piscului 1970 - Dealul Piscului 1970/ Stereo 70 (deprecated)
    (31700, 19.031103, 41.338741, 1e-4, 0.0, 0.0, 1e-07),
    # NGN - NGN / UTM zone 38N
    (31838, 40.511236, 0.000025, 1e-4, 0.0, 0.0, 0.000167729129317),
    # NGN - NGN / UTM zone 39N
    (31839, 46.511242, 0.000025, 1e-4, 0.0, 0.0, 0.000167729129317),
    # KUDAMS - KUDAMS / KTM (deprecated)
    (31900, 43.511458, 0.000022, 1e-4, 0.0, 0.0, 0.000167729216628),
    # KUDAMS - KUDAMS / KTM
    (31901, 43.513250, 0.000022, 1e-4, 0.0, 0.0, 0.000167407561094),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 11N
    (31965, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 12N
    (31966, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 13N
    (31967, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 14N
    (31968, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 15N
    (31969, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 16N
    (31970, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167730002431),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 17N
    (31971, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 18N
    (31972, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 19N
    (31973, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 20N
    (31974, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 21N
    (31975, -61.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 22N
    (31976, -55.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 17S
    (31977, -81.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 18S
    (31978, -75.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 19S
    (31979, -69.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 20S
    (31980, -63.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 21S
    (31981, -57.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 22S
    (31982, -51.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 23S
    (31983, -45.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 24S
    (31984, -39.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 2000 - SIRGAS 2000 / UTM zone 25S
    (31985, -33.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 17N
    (31986, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 18N
    (31987, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 19N
    (31988, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 20N
    (31989, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 21N
    (31990, -61.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727819644),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 22N
    (31991, -55.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728954693),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 17S
    (31992, -81.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 18S
    (31993, -75.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 19S
    (31994, -69.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 20S
    (31995, -63.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 21S
    (31996, -57.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 22S
    (31997, -51.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 23S
    (31998, -45.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 24S
    (31999, -39.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # SIRGAS 1995 - SIRGAS 1995 / UTM zone 25S
    (32000, -33.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # NAD27 - NAD27 / Montana North
    (32001, -117.484233, 46.714622, 2e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Montana Central
    (32002, -117.318095, 45.558750, 2e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Montana South
    (32003, -117.075191, 43.740616, 2e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Nebraska North
    (32005, -107.263569, 41.098909, 2e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Nebraska South
    (32006, -106.587140, 39.443804, 2e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Nevada East
    (32007, -117.247546, 34.738628, 1e-3, 0.0, 0.0, 1.5117996991e-06),
    # NAD27 - NAD27 / Nevada Central
    (32008, -118.330880, 34.738628, 1e-3, 0.0, 0.0, 1.5117996991e-06),
    # NAD27 - NAD27 / Nevada West
    (32009, -120.247546, 34.738628, 1e-3, 0.0, 0.0, 1.5117996991e-06),
    # NAD27 - NAD27 / New Hampshire
    (32010, -73.520246, 42.485008, 1e-3, 0.0, 0.0, 3.03175176045e-06),
    # NAD27 - NAD27 / New Jersey
    (32011, -81.662991, 38.623462, 1e-3, 0.0, 0.0, 0.00441632172762),
    # NAD27 - NAD27 / New Mexico East
    (32012, -105.928927, 30.990142, 1e-3, 0.0, 0.0, 1.03341090323e-06),
    # NAD27 - NAD27 / New Mexico Central
    (32013, -107.845608, 30.990142, 1e-3, 0.0, 0.0, 1.02763225759e-06),
    # NAD27 - NAD27 / New Mexico West
    (32014, -109.428915, 30.990142, 1e-3, 0.0, 0.0, 1.02998075549e-06),
    # NAD27 - NAD27 / New York East
    (32015, -76.117613, 39.986263, 1e-3, 0.0, 0.0, 2.42848375019e-06),
    # NAD27 - NAD27 / New York Central
    (32016, -78.367665, 39.986263, 1e-3, 0.0, 0.0, 2.43385319287e-06),
    # NAD27 - NAD27 / New York West
    (32017, -80.367665, 39.986263, 1e-3, 0.0, 0.0, 2.43385319287e-06),
    # NAD27 - NAD27 / New York Long Island (deprecated)
    (32018, -77.593552, 40.443690, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / North Carolina
    (32019, -85.568145, 33.568153, 1e-3, 0.0, 0.0, 2.37353193071e-07),
    # NAD27 - NAD27 / North Dakota North
    (32020, -108.485256, 46.715440, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / North Dakota South
    (32021, -108.295673, 45.394334, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Ohio North
    (32022, -89.586786, 39.443529, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Ohio South
    (32023, -89.425158, 37.789345, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Oklahoma North
    (32024, -104.666110, 34.811312, 1e-3, 0.0, 0.0, 2.07850452923e-07),
    # NAD27 - NAD27 / Oklahoma South
    (32025, -104.537369, 33.155317, 1e-3, 0.0, 0.0, 2.4728223516e-07),
    # NAD27 - NAD27 / Oregon North
    (32026, -128.034335, 43.410712, 2e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Oregon South
    (32027, -127.799293, 41.427388, 2e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Pennsylvania North
    (32028, -84.888354, 39.940340, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Pennsylvania South (deprecated)
    (32029, -84.804311, 39.114318, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Rhode Island
    (32030, -73.313189, 41.069065, 1e-3, 0.0, 0.0, 2.67450877702e-06),
    # NAD27 - NAD27 / South Carolina North
    (32031, -87.512485, 32.823637, 1e-3, 0.0, 0.0, 2.75639157504e-07),
    # NAD27 - NAD27 / South Carolina South
    (32033, -87.430604, 31.665268, 1e-3, 0.0, 0.0, 2.82035950804e-07),
    # NAD27 - NAD27 / South Dakota North
    (32034, -107.555775, 43.577161, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / South Dakota South
    (32035, -107.709076, 42.089535, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Tennessee (deprecated)
    (32036, -86.332485, 34.666200, 1e-3, 0.0, 0.0, 2.07581901875e-07),
    # NAD27 - NAD27 / Texas North
    (32037, -108.087177, 33.816885, 1e-3, 0.0, 0.0, 2.35634466364e-07),
    # NAD27 - NAD27 / Texas North Central
    (32038, -103.918952, 31.498678, 1e-3, 0.0, 0.0, 2.86714017398e-07),
    # NAD27 - NAD27 / Texas Central
    (32039, -106.622621, 29.511183, 1e-3, 0.0, 0.0, 3.40085204042e-07),
    # NAD27 - NAD27 / Texas South Central
    (32040, -105.180905, 27.687962, 1e-3, 0.0, 0.0, 3.81330508359e-07),
    # NAD27 - NAD27 / Texas South
    (32041, -104.566035, 25.534412, 1e-3, 0.0, 0.0, 4.34223581474e-07),
    # NAD27 - NAD27 / Utah North
    (32042, -118.656687, 40.107155, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Utah Central
    (32043, -118.456280, 38.119724, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Utah South
    (32044, -118.305745, 36.466713, 1e-3, 0.0, 0.0, 1.78731030932e-07),
    # NAD27 - NAD27 / Vermont
    (32045, -74.353583, 42.485008, 1e-3, 0.0, 0.0, 3.03181254819e-06),
    # NAD27 - NAD27 / Virginia North
    (32046, -85.395588, 37.460310, 1e-3, 0.0, 0.0, 1.58967742127e-07),
    # NAD27 - NAD27 / Virginia South
    (32047, -85.277184, 36.136075, 1e-3, 0.0, 0.0, 1.8455491533e-07),
    # NAD27 - NAD27 / Washington North
    (32048, -128.818421, 46.715304, 2e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Washington South
    (32049, -128.250243, 45.063688, 2e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / West Virginia North
    (32050, -86.473077, 38.286794, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / West Virginia South
    (32051, -87.835029, 36.797389, 1e-3, 0.0, 0.0, 1.73480055436e-07),
    # NAD27 - NAD27 / Wisconsin North
    (32052, -97.728511, 44.899617, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Wisconsin Central
    (32053, -97.556344, 43.577908, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Wisconsin South
    (32054, -97.336949, 41.758457, 1e-3, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Wyoming East
    (32055, -106.968625, 40.652604, 1e-3, 0.0, 0.0, 2.57514843821e-06),
    # NAD27 - NAD27 / Wyoming East Central
    (32056, -109.135292, 40.652604, 1e-3, 0.0, 0.0, 2.57514843821e-06),
    # NAD27 - NAD27 / Wyoming West Central
    (32057, -110.551959, 40.652604, 1e-3, 0.0, 0.0, 2.56995792606e-06),
    # NAD27 - NAD27 / Wyoming West
    (32058, -111.885292, 40.652604, 1e-3, 0.0, 0.0, 2.57514843821e-06),
    # NAD27 - NAD27 / Guatemala Norte (deprecated)
    (32061, -94.959657, 14.123746, 1e-3, 0.0, 0.0, 1.61264324561e-07),
    # NAD27 - NAD27 / Guatemala Sur (deprecated)
    (32062, -94.917576, 11.907800, 1e-4, 0.0, 0.0, 1e-05),
    # NAD27 - NAD27 / BLM 14N (ftUS)
    (32064, -103.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 15N (ftUS)
    (32065, -97.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560095508481),
    # NAD27 - NAD27 / BLM 16N (ftUS)
    (32066, -91.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560095508481),
    # NAD27 - NAD27 / BLM 17N (ftUS)
    (32067, -85.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560095508481),
    # NAD27 - NAD27 / BLM 14N (feet) (deprecated)
    (32074, -103.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560102383388),
    # NAD27 - NAD27 / BLM 15N (feet) (deprecated)
    (32075, -97.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560095508481),
    # NAD27 - NAD27 / BLM 16N (feet) (deprecated)
    (32076, -91.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560095508481),
    # NAD27 - NAD27 / BLM 17N (feet) (deprecated)
    (32077, -85.488695, 0.0, 1e-4, 0.0, 0.0, 0.000560095508481),
    # NAD27 - NAD27 / MTM zone 1
    (32081, -55.737260, 0.0, 1e-4, 0.0, 0.0, 1.09803804662e-05),
    # NAD27 - NAD27 / MTM zone 2
    (32082, -58.737260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27 - NAD27 / MTM zone 3
    (32083, -61.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27 - NAD27 / MTM zone 4
    (32084, -64.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27 - NAD27 / MTM zone 5
    (32085, -67.237260, 0.0, 1e-4, 0.0, 0.0, 1.09814282041e-05),
    # NAD27 - NAD27 / MTM zone 6
    (32086, -70.237260, 0.0, 1e-4, 0.0, 0.0, 1.09836109914e-05),
    # NAD27 - NAD27 / Quebec Lambert
    (32098, -68.500000, 44.0, 6e-4, 0.0, 0.0, 1e-07),
    # NAD27 - NAD27 / Louisiana Offshore
    (32099, -97.399368, 25.534412, 4e-4, 0.0, 0.0, 4.48068190507e-07),
    # NAD83 - NAD83 / Montana
    (32100, -116.985465, 43.991943, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Nebraska
    (32104, -105.831722, 39.681418, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Nevada East
    (32107, -117.845072, -37.495818, 1e-4, 0.0, 0.0, 2.70277087111e-06),
    # NAD83 - NAD83 / Nevada Central
    (32108, -121.423274, -19.408016, 1e-4, 0.0, 0.0, 3.61900019925e-05),
    # NAD83 - NAD83 / Nevada West
    (32109, -125.753742, -1.377653, 1e-4, 0.0, 0.0, 0.00310022570193),
    # NAD83 - NAD83 / New Hampshire
    (32110, -75.312874, 42.441965, 1e-4, 0.0, 0.0, 4.28260943867e-05),
    # NAD83 - NAD83 / New Jersey
    (32111, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 6.02422172344e-07),
    # NAD83 - NAD83 / New Mexico East
    (32112, -106.060830, 30.988445, 1e-4, 0.0, 0.0, 4.79974244135e-07),
    # NAD83 - NAD83 / New Mexico Central
    (32113, -111.476675, 30.894188, 1e-4, 0.0, 0.0, 0.000274516973348),
    # NAD83 - NAD83 / New Mexico West
    (32114, -116.482847, 30.710010, 1e-4, 0.0, 0.0, 0.00241149640912),
    # NAD83 - NAD83 / New York East
    (32115, -76.227336, 38.820562, 1e-4, 0.0, 0.0, 6.02422172344e-07),
    # NAD83 - NAD83 / New York Central
    (32116, -79.509326, 39.963054, 1e-4, 0.0, 0.0, 1.20960787919e-05),
    # NAD83 - NAD83 / New York West
    (32117, -82.677307, 39.927648, 1e-4, 0.0, 0.0, 8.10902805652e-05),
    # NAD83 - NAD83 / New York Long Island
    (32118, -77.519584, 40.112385, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / North Carolina
    (32119, -85.568291, 33.568155, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / North Dakota North
    (32120, -108.360607, 46.724303, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / North Dakota South
    (32121, -108.173906, 45.402819, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Ohio North
    (32122, -89.475829, 39.450489, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Ohio South
    (32123, -89.316674, 37.795918, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Oklahoma North
    (32124, -104.561592, 34.817203, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Oklahoma South
    (32125, -104.434828, 33.160876, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Oregon North
    (32126, -150.091081, 39.506127, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Oregon South
    (32127, -138.236892, 40.231913, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Pennsylvania North
    (32128, -84.776606, 39.947400, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Pennsylvania South
    (32129, -84.693691, 39.120795, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Rhode Island
    (32130, -72.689948, 41.077189, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / South Carolina
    (32133, -87.429394, 31.662328, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / South Dakota North
    (32134, -107.437658, 43.585145, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / South Dakota South
    (32135, -107.593704, 42.097136, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Tennessee
    (32136, -92.508665, 34.153466, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Texas North
    (32137, -103.450598, 25.015254, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Texas North Central
    (32138, -103.763988, 13.819848, 1e-4, 0.0, 0.0, 1.59605406225e-07),
    # NAD83 - NAD83 / Texas Central
    (32139, -105.983204, 3.442337, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Texas South Central
    (32140, -103.518030, -6.145758, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Texas South
    (32141, -100.642123, -15.495006, 1e-4, 0.0, 0.0, 2.08325218409e-07),
    # NAD83 - NAD83 / Utah North
    (32142, -116.675635, 31.235394, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Utah Central
    (32143, -116.049199, 20.524350, 1e-4, 0.0, 0.0, 1.61089701578e-07),
    # NAD83 - NAD83 / Utah South
    (32144, -115.612862, 10.480191, 1e-4, 0.0, 0.0, 1.55065208673e-07),
    # NAD83 - NAD83 / Vermont
    (32145, -78.566435, 42.339184, 1e-4, 0.0, 0.0, 0.000677358422587),
    # NAD83 - NAD83 / Virginia North
    (32146, -109.123093, 14.944862, 1e-4, 0.0, 0.0, 2.08849087358e-07),
    # NAD83 - NAD83 / Virginia South
    (32147, -111.898764, 21.848820, 1e-4, 0.0, 0.0, 1.84925738722e-07),
    # NAD83 - NAD83 / Washington North
    (32148, -127.390662, 46.808297, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Washington South
    (32149, -126.863697, 45.151783, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / West Virginia North
    (32150, -86.363854, 38.293447, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / West Virginia South
    (32151, -87.727921, 36.803713, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Wisconsin North
    (32152, -97.607760, 44.907938, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Wisconsin Central
    (32153, -97.438214, 43.585869, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Wisconsin South
    (32154, -97.222171, 41.765988, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Wyoming East
    (32155, -107.525253, 40.475927, 1e-4, 0.0, 0.0, 3.54957583129e-06),
    # NAD83 - NAD83 / Wyoming East Central
    (32156, -111.983494, 39.506205, 1e-4, 0.0, 0.0, 0.000162927870406),
    # NAD83 - NAD83 / Wyoming West Central
    (32157, -115.803136, 40.284354, 1e-4, 0.0, 0.0, 0.00182561155378),
    # NAD83 - NAD83 / Wyoming West
    (32158, -119.340930, 39.229368, 1e-4, 0.0, 0.0, 0.0180613152843),
    # NAD83 - NAD83 / Puerto Rico & Virgin Is.
    (32161, -68.300715, 16.017453, 1e-4, 0.0, 0.0, 1.52795109898e-07),
    # NAD83 - NAD83 / BLM 14N (ftUS)
    (32164, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550296474612),
    # NAD83 - NAD83 / BLM 15N (ftUS)
    (32165, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / BLM 16N (ftUS)
    (32166, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550296474612),
    # NAD83 - NAD83 / BLM 17N (ftUS)
    (32167, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # NAD83 - NAD83 / SCoPQ zone 2 (deprecated)
    (32180, -58.237290, 0.0, 1e-4, 0.0, 0.0, 1.07595697045e-05),
    # NAD83 - NAD83 / MTM zone 1
    (32181, -55.737290, 0.0, 1e-4, 0.0, 0.0, 1.07595697045e-05),
    # NAD83 - NAD83 / MTM zone 2
    (32182, -58.737290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 3
    (32183, -61.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 4
    (32184, -64.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 5
    (32185, -67.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 6
    (32186, -70.237290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83 - NAD83 / MTM zone 7
    (32187, -73.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 8
    (32188, -76.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 9
    (32189, -79.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 10
    (32190, -82.237290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83 - NAD83 / MTM zone 11
    (32191, -85.237290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 12
    (32192, -83.737290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 13
    (32193, -86.737290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83 - NAD83 / MTM zone 14
    (32194, -89.737290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 15
    (32195, -92.737290, 0.0, 1e-4, 0.0, 0.0, 1.07606174424e-05),
    # NAD83 - NAD83 / MTM zone 16
    (32196, -95.737290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83 - NAD83 / MTM zone 17
    (32197, -98.737290, 0.0, 1e-4, 0.0, 0.0, 1.07628002297e-05),
    # NAD83 - NAD83 / Quebec Lambert
    (32198, -68.500000, 44.0, 1e-4, 0.0, 0.0, 1e-07),
    # NAD83 - NAD83 / Louisiana Offshore
    (32199, -101.257010, 25.145011, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 72 - WGS 72 / UTM zone 1N
    (32201, 178.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584981),
    # WGS 72 - WGS 72 / UTM zone 2N
    (32202, -175.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236748863244),
    # WGS 72 - WGS 72 / UTM zone 3N
    (32203, -169.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236745981965),
    # WGS 72 - WGS 72 / UTM zone 4N
    (32204, -163.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584981),
    # WGS 72 - WGS 72 / UTM zone 5N
    (32205, -157.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584981),
    # WGS 72 - WGS 72 / UTM zone 6N
    (32206, -151.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584981),
    # WGS 72 - WGS 72 / UTM zone 7N
    (32207, -145.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236745981965),
    # WGS 72 - WGS 72 / UTM zone 8N
    (32208, -139.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584983),
    # WGS 72 - WGS 72 / UTM zone 9N
    (32209, -133.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236748863244),
    # WGS 72 - WGS 72 / UTM zone 10N
    (32210, -127.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584981),
    # WGS 72 - WGS 72 / UTM zone 11N
    (32211, -121.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584983),
    # WGS 72 - WGS 72 / UTM zone 12N
    (32212, -115.488591, 0.000041, 1e-4, 0.0, 0.0, 0.00023674100521),
    # WGS 72 - WGS 72 / UTM zone 13N
    (32213, -109.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 14N
    (32214, -103.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 15N
    (32215, -97.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236745981965),
    # WGS 72 - WGS 72 / UTM zone 16N
    (32216, -91.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 17N
    (32217, -85.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 18N
    (32218, -79.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886489),
    # WGS 72 - WGS 72 / UTM zone 19N
    (32219, -73.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 20N
    (32220, -67.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886489),
    # WGS 72 - WGS 72 / UTM zone 21N
    (32221, -61.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886489),
    # WGS 72 - WGS 72 / UTM zone 22N
    (32222, -55.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886491),
    # WGS 72 - WGS 72 / UTM zone 23N
    (32223, -49.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 24N
    (32224, -43.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236740044785),
    # WGS 72 - WGS 72 / UTM zone 25N
    (32225, -37.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584981),
    # WGS 72 - WGS 72 / UTM zone 26N
    (32226, -31.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 27N
    (32227, -25.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 28N
    (32228, -19.488591, 0.000041, 1e-4, 0.0, 0.0, 0.00023674441036),
    # WGS 72 - WGS 72 / UTM zone 29N
    (32229, -13.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236743275309),
    # WGS 72 - WGS 72 / UTM zone 30N
    (32230, -7.488591, 0.000041, 1e-4, 0.0, 0.0, 0.000236744148424),
    # WGS 72 - WGS 72 / UTM zone 31N
    (32231, -1.488591, 0.000041, 1e-4, 0.0, 0.0, 0.00023674275144),
    # WGS 72 - WGS 72 / UTM zone 32N
    (32232, 4.511409, 0.000041, 1e-4, 0.0, 0.0, 0.00023674275144),
    # WGS 72 - WGS 72 / UTM zone 33N
    (32233, 10.511409, 0.000041, 1e-4, 0.0, 0.0, 0.00023674275144),
    # WGS 72 - WGS 72 / UTM zone 34N
    (32234, 16.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236741965637),
    # WGS 72 - WGS 72 / UTM zone 35N
    (32235, 22.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886489),
    # WGS 72 - WGS 72 / UTM zone 36N
    (32236, 28.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236743013375),
    # WGS 72 - WGS 72 / UTM zone 37N
    (32237, 34.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886489),
    # WGS 72 - WGS 72 / UTM zone 38N
    (32238, 40.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 39N
    (32239, 46.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236742838752),
    # WGS 72 - WGS 72 / UTM zone 40N
    (32240, 52.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886489),
    # WGS 72 - WGS 72 / UTM zone 41N
    (32241, 58.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236745283473),
    # WGS 72 - WGS 72 / UTM zone 42N
    (32242, 64.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236741791015),
    # WGS 72 - WGS 72 / UTM zone 43N
    (32243, 70.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886491),
    # WGS 72 - WGS 72 / UTM zone 44N
    (32244, 76.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 45N
    (32245, 82.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886489),
    # WGS 72 - WGS 72 / UTM zone 46N
    (32246, 88.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 47N
    (32247, 94.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236742489506),
    # WGS 72 - WGS 72 / UTM zone 48N
    (32248, 100.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886489),
    # WGS 72 - WGS 72 / UTM zone 49N
    (32249, 106.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236743886489),
    # WGS 72 - WGS 72 / UTM zone 50N
    (32250, 112.511409, 0.000041, 1e-4, 0.0, 0.0, 0.00023674039403),
    # WGS 72 - WGS 72 / UTM zone 51N
    (32251, 118.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236743187997),
    # WGS 72 - WGS 72 / UTM zone 52N
    (32252, 124.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236745981965),
    # WGS 72 - WGS 72 / UTM zone 53N
    (32253, 130.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236745981965),
    # WGS 72 - WGS 72 / UTM zone 54N
    (32254, 136.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584981),
    # WGS 72 - WGS 72 / UTM zone 55N
    (32255, 142.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236745981967),
    # WGS 72 - WGS 72 / UTM zone 56N
    (32256, 148.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236745981965),
    # WGS 72 - WGS 72 / UTM zone 57N
    (32257, 154.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236741791014),
    # WGS 72 - WGS 72 / UTM zone 58N
    (32258, 160.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584983),
    # WGS 72 - WGS 72 / UTM zone 59N
    (32259, 166.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584981),
    # WGS 72 - WGS 72 / UTM zone 60N
    (32260, 172.511409, 0.000041, 1e-4, 0.0, 0.0, 0.000236744584981),
    # WGS 72 - WGS 72 / UTM zone 1S
    (32301, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 2S
    (32302, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 3S
    (32303, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 4S
    (32304, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 5S
    (32305, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 6S
    (32306, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 7S
    (32307, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 8S
    (32308, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 9S
    (32309, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 10S
    (32310, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 11S
    (32311, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 12S
    (32312, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 13S
    (32313, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 14S
    (32314, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 15S
    (32315, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 16S
    (32316, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 17S
    (32317, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 18S
    (32318, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 19S
    (32319, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 20S
    (32320, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 21S
    (32321, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 22S
    (32322, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 23S
    (32323, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 24S
    (32324, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 25S
    (32325, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 26S
    (32326, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 27S
    (32327, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 28S
    (32328, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 29S
    (32329, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 30S
    (32330, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 31S
    (32331, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 32S
    (32332, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 33S
    (32333, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 34S
    (32334, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 35S
    (32335, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 36S
    (32336, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 37S
    (32337, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 38S
    (32338, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 39S
    (32339, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 40S
    (32340, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 41S
    (32341, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 42S
    (32342, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 43S
    (32343, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 44S
    (32344, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 45S
    (32345, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72 - WGS 72 / UTM zone 46S
    (32346, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 47S
    (32347, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 48S
    (32348, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 49S
    (32349, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 50S
    (32350, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 51S
    (32351, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 52S
    (32352, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 53S
    (32353, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 54S
    (32354, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 55S
    (32355, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 56S
    (32356, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 57S
    (32357, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 58S
    (32358, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 59S
    (32359, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72 - WGS 72 / UTM zone 60S
    (32360, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 1N
    (32401, 178.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316727499013),
    # WGS 72BE - WGS 72BE / UTM zone 2N
    (32402, -175.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316731777276),
    # WGS 72BE - WGS 72BE / UTM zone 3N
    (32403, -169.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316728895996),
    # WGS 72BE - WGS 72BE / UTM zone 4N
    (32404, -163.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316724705044),
    # WGS 72BE - WGS 72BE / UTM zone 5N
    (32405, -157.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316728983307),
    # WGS 72BE - WGS 72BE / UTM zone 6N
    (32406, -151.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316724705044),
    # WGS 72BE - WGS 72BE / UTM zone 7N
    (32407, -145.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316728895996),
    # WGS 72BE - WGS 72BE / UTM zone 8N
    (32408, -139.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316724705044),
    # WGS 72BE - WGS 72BE / UTM zone 9N
    (32409, -133.488519, 0.000017, 1e-4, 0.0, 0.0, 0.00031673317426),
    # WGS 72BE - WGS 72BE / UTM zone 10N
    (32410, -127.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316731777276),
    # WGS 72BE - WGS 72BE / UTM zone 11N
    (32411, -121.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316728895996),
    # WGS 72BE - WGS 72BE / UTM zone 12N
    (32412, -115.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316725403537),
    # WGS 72BE - WGS 72BE / UTM zone 13N
    (32413, -109.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316724705044),
    # WGS 72BE - WGS 72BE / UTM zone 14N
    (32414, -103.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316720514093),
    # WGS 72BE - WGS 72BE / UTM zone 15N
    (32415, -97.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316727499013),
    # WGS 72BE - WGS 72BE / UTM zone 16N
    (32416, -91.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316725403537),
    # WGS 72BE - WGS 72BE / UTM zone 17N
    (32417, -85.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316726102029),
    # WGS 72BE - WGS 72BE / UTM zone 18N
    (32418, -79.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316727499013),
    # WGS 72BE - WGS 72BE / UTM zone 19N
    (32419, -73.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316722609568),
    # WGS 72BE - WGS 72BE / UTM zone 20N
    (32420, -67.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316726887831),
    # WGS 72BE - WGS 72BE / UTM zone 21N
    (32421, -61.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316725403537),
    # WGS 72BE - WGS 72BE / UTM zone 22N
    (32422, -55.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316726451275),
    # WGS 72BE - WGS 72BE / UTM zone 23N
    (32423, -49.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316724705044),
    # WGS 72BE - WGS 72BE / UTM zone 24N
    (32424, -43.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316723657306),
    # WGS 72BE - WGS 72BE / UTM zone 25N
    (32425, -37.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316725403537),
    # WGS 72BE - WGS 72BE / UTM zone 26N
    (32426, -31.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316723657306),
    # WGS 72BE - WGS 72BE / UTM zone 27N
    (32427, -25.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316723133437),
    # WGS 72BE - WGS 72BE / UTM zone 28N
    (32428, -19.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316724006552),
    # WGS 72BE - WGS 72BE / UTM zone 29N
    (32429, -13.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316722609568),
    # WGS 72BE - WGS 72BE / UTM zone 30N
    (32430, -7.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316725665471),
    # WGS 72BE - WGS 72BE / UTM zone 31N
    (32431, -1.488519, 0.000017, 1e-4, 0.0, 0.0, 0.000316725665471),
    # WGS 72BE - WGS 72BE / UTM zone 32N
    (32432, 4.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316724355799),
    # WGS 72BE - WGS 72BE / UTM zone 33N
    (32433, 10.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316725927406),
    # WGS 72BE - WGS 72BE / UTM zone 34N
    (32434, 16.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316723133437),
    # WGS 72BE - WGS 72BE / UTM zone 35N
    (32435, 22.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316723657306),
    # WGS 72BE - WGS 72BE / UTM zone 36N
    (32436, 28.511481, 0.000017, 1e-4, 0.0, 0.0, 0.00031672557816),
    # WGS 72BE - WGS 72BE / UTM zone 37N
    (32437, 34.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316720514093),
    # WGS 72BE - WGS 72BE / UTM zone 38N
    (32438, 40.511481, 0.000017, 1e-4, 0.0, 0.0, 0.00031672156183),
    # WGS 72BE - WGS 72BE / UTM zone 39N
    (32439, 46.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316724705045),
    # WGS 72BE - WGS 72BE / UTM zone 40N
    (32440, 52.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316722958815),
    # WGS 72BE - WGS 72BE / UTM zone 41N
    (32441, 58.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316726800521),
    # WGS 72BE - WGS 72BE / UTM zone 42N
    (32442, 64.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316726800521),
    # WGS 72BE - WGS 72BE / UTM zone 43N
    (32443, 70.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316724006552),
    # WGS 72BE - WGS 72BE / UTM zone 44N
    (32444, 76.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316721911076),
    # WGS 72BE - WGS 72BE / UTM zone 45N
    (32445, 82.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316723308061),
    # WGS 72BE - WGS 72BE / UTM zone 46N
    (32446, 88.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316725403537),
    # WGS 72BE - WGS 72BE / UTM zone 47N
    (32447, 94.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316726102029),
    # WGS 72BE - WGS 72BE / UTM zone 48N
    (32448, 100.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316727499013),
    # WGS 72BE - WGS 72BE / UTM zone 49N
    (32449, 106.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316726102028),
    # WGS 72BE - WGS 72BE / UTM zone 50N
    (32450, 112.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316720514093),
    # WGS 72BE - WGS 72BE / UTM zone 51N
    (32451, 118.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316724705044),
    # WGS 72BE - WGS 72BE / UTM zone 52N
    (32452, 124.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316728895996),
    # WGS 72BE - WGS 72BE / UTM zone 53N
    (32453, 130.511481, 0.000017, 1e-4, 0.0, 0.0, 0.00031672330806),
    # WGS 72BE - WGS 72BE / UTM zone 54N
    (32454, 136.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316724705045),
    # WGS 72BE - WGS 72BE / UTM zone 55N
    (32455, 142.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316723308061),
    # WGS 72BE - WGS 72BE / UTM zone 56N
    (32456, 148.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316728895996),
    # WGS 72BE - WGS 72BE / UTM zone 57N
    (32457, 154.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316726102028),
    # WGS 72BE - WGS 72BE / UTM zone 58N
    (32458, 160.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316721911076),
    # WGS 72BE - WGS 72BE / UTM zone 59N
    (32459, 166.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316724705044),
    # WGS 72BE - WGS 72BE / UTM zone 60N
    (32460, 172.511481, 0.000017, 1e-4, 0.0, 0.0, 0.000316727499013),
    # WGS 72BE - WGS 72BE / UTM zone 1S
    (32501, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 2S
    (32502, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 3S
    (32503, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 4S
    (32504, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 5S
    (32505, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 6S
    (32506, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 7S
    (32507, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 8S
    (32508, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 9S
    (32509, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 10S
    (32510, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 11S
    (32511, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 12S
    (32512, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 13S
    (32513, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 14S
    (32514, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 15S
    (32515, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 16S
    (32516, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 17S
    (32517, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 18S
    (32518, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 19S
    (32519, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 20S
    (32520, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 21S
    (32521, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 22S
    (32522, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 23S
    (32523, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 24S
    (32524, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 25S
    (32525, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 26S
    (32526, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 27S
    (32527, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 28S
    (32528, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 29S
    (32529, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 30S
    (32530, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 31S
    (32531, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 32S
    (32532, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 33S
    (32533, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 34S
    (32534, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 35S
    (32535, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 36S
    (32536, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 37S
    (32537, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 38S
    (32538, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 39S
    (32539, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 40S
    (32540, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 41S
    (32541, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 42S
    (32542, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 43S
    (32543, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 44S
    (32544, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 45S
    (32545, 0.0, -90.0, 1e-4, 0.0, 0.0, 753058),
    # WGS 72BE - WGS 72BE / UTM zone 46S
    (32546, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 47S
    (32547, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 48S
    (32548, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 49S
    (32549, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 50S
    (32550, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 51S
    (32551, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 52S
    (32552, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 53S
    (32553, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 54S
    (32554, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 55S
    (32555, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 56S
    (32556, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 57S
    (32557, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 58S
    (32558, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 59S
    (32559, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 72BE - WGS 72BE / UTM zone 60S
    (32560, 0.0, -90.0, 1e-4, 0.0, 0.0, 3),
    # WGS 84 - WGS 84 / UTM zone 1N
    (32601, 178.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167725549545),
    # WGS 84 - WGS 84 / UTM zone 2N
    (32602, -175.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 3N
    (32603, -169.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167734106071),
    # WGS 84 - WGS 84 / UTM zone 4N
    (32604, -163.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 5N
    (32605, -157.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 6N
    (32606, -151.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167734106071),
    # WGS 84 - WGS 84 / UTM zone 7N
    (32607, -145.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 8N
    (32608, -139.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 9N
    (32609, -133.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 10N
    (32610, -127.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 11N
    (32611, -121.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 12N
    (32612, -115.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 13N
    (32613, -109.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 14N
    (32614, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 15N
    (32615, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 16N
    (32616, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 17N
    (32617, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 18N
    (32618, -79.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 19N
    (32619, -73.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 20N
    (32620, -67.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 21N
    (32621, -61.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 22N
    (32622, -55.488744, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 23N
    (32623, -49.488744, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 24N
    (32624, -43.488744, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 25N
    (32625, -37.488744, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 26N
    (32626, -31.488744, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 27N
    (32627, -25.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167729303939),
    # WGS 84 - WGS 84 / UTM zone 28N
    (32628, -19.488744, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 29N
    (32629, -13.488744, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 30N
    (32630, -7.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728518136),
    # WGS 84 - WGS 84 / UTM zone 31N
    (32631, -1.488744, 0.0, 1e-4, 0.0, 0.0, 0.000167728518136),
    # WGS 84 - WGS 84 / UTM zone 32N
    (32632, 4.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728518136),
    # WGS 84 - WGS 84 / UTM zone 33N
    (32633, 10.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167728518136),
    # WGS 84 - WGS 84 / UTM zone 34N
    (32634, 16.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 35N
    (32635, 22.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 36N
    (32636, 28.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 37N
    (32637, 34.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 38N
    (32638, 40.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 39N
    (32639, 46.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 40N
    (32640, 52.511256, 0.0, 1e-4, 0.0, 0.0, 0.00016772878007),
    # WGS 84 - WGS 84 / UTM zone 41N
    (32641, 58.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 42N
    (32642, 64.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 43N
    (32643, 70.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 44N
    (32644, 76.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 45N
    (32645, 82.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 46N
    (32646, 88.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 47N
    (32647, 94.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 48N
    (32648, 100.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167725549545),
    # WGS 84 - WGS 84 / UTM zone 49N
    (32649, 106.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 50N
    (32650, 112.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167727732332),
    # WGS 84 - WGS 84 / UTM zone 51N
    (32651, 118.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 52N
    (32652, 124.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 53N
    (32653, 130.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 54N
    (32654, 136.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 55N
    (32655, 142.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167725549545),
    # WGS 84 - WGS 84 / UTM zone 56N
    (32656, 148.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 57N
    (32657, 154.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 58N
    (32658, 160.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UTM zone 59N
    (32659, 166.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167725549545),
    # WGS 84 - WGS 84 / UTM zone 60N
    (32660, 172.511256, 0.0, 1e-4, 0.0, 0.0, 0.000167729827808),
    # WGS 84 - WGS 84 / UPS North (N,E)
    (32661, -45.0, 64.916412, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / Plate Carree (deprecated)
    (32662, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / World Equidistant Cylindrical (deprecated)
    # (32663, 0.0, 0.0, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / BLM 14N (ftUS)
    (32664, -103.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550296474612),
    # WGS 84 - WGS 84 / BLM 15N (ftUS)
    (32665, -97.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # WGS 84 - WGS 84 / BLM 16N (ftUS)
    (32666, -91.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # WGS 84 - WGS 84 / BLM 17N (ftUS)
    (32667, -85.488744, 0.0, 1e-4, 0.0, 0.0, 0.000550289599705),
    # WGS 84 - WGS 84 / UTM zone 1S
    (32701, -177.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 2S
    (32702, -171.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 3S
    (32703, -165.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 4S
    (32704, -159.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 5S
    (32705, -153.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 6S
    (32706, -147.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 7S
    (32707, -141.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 8S
    (32708, -135.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 9S
    (32709, -129.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 10S
    (32710, -123.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 11S
    (32711, -117.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 12S
    (32712, -111.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 13S
    (32713, -105.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 14S
    (32714, -99.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 15S
    (32715, -93.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 16S
    (32716, -87.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 17S
    (32717, -81.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 18S
    (32718, -75.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 19S
    (32719, -69.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 20S
    (32720, -63.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 21S
    (32721, -57.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 22S
    (32722, -51.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 23S
    (32723, -45.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 24S
    (32724, -39.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 25S
    (32725, -33.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 26S
    (32726, -27.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 27S
    (32727, -21.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 28S
    (32728, -15.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 29S
    (32729, -9.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 30S
    (32730, -3.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 31S
    (32731, 3.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 32S
    (32732, 9.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 33S
    (32733, 15.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 34S
    (32734, 21.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 35S
    (32735, 27.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 36S
    (32736, 33.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 37S
    (32737, 39.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 38S
    (32738, 45.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 39S
    (32739, 51.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 40S
    (32740, 57.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 41S
    (32741, 63.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 42S
    (32742, 69.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 43S
    (32743, 75.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 44S
    (32744, 81.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 45S
    (32745, 87.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 46S
    (32746, 93.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 47S
    (32747, 99.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 48S
    (32748, 105.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 49S
    (32749, 111.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 50S
    (32750, 117.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 51S
    (32751, 123.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 52S
    (32752, 129.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 53S
    (32753, 135.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 54S
    (32754, 141.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 55S
    (32755, 147.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 56S
    (32756, 153.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 57S
    (32757, 159.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 58S
    (32758, 165.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 59S
    (32759, 171.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UTM zone 60S
    (32760, 177.0, -90.0, 1e-4, 0.0, 0.0, 753053),
    # WGS 84 - WGS 84 / UPS South (N,E)
    (32761, -135.0, -64.916412, 1e-4, 0.0, 0.0, 1e-07),
    # WGS 84 - WGS 84 / TM 36 SE
    (32766, 36.0, -90.0, 1e-4, 0.0, 0.0, 753053),
]

@unittest.skipIf(not osr_util.HaveProj4(), 'requires PROJ.4')
class OsrCt(unittest.TestCase):

  def setUp(self):
    self.utm_srs = osr.SpatialReference()
    self.utm_srs.SetUTM(11)
    self.utm_srs.SetWellKnownGeogCS('WGS84')

    self.ll_srs = osr.SpatialReference()
    self.ll_srs.SetWellKnownGeogCS('WGS84')

    self.ct = osr.CoordinateTransformation(self.ll_srs, self.utm_srs)

  # Test 1 is wrapped into the skipIf.

  def testCt02SimpleLlUtm(self):
    dst_xyz = self.ct.TransformPoint(-117.5, 32., 0.)
    expected_xyz = (452772.0628547681, 3540544.8958501634, 0.)
    for i in range(3):
      self.assertAlmostEqual(dst_xyz[i], expected_xyz[i])

  def testCt03PreserveSrcSrs(self):
    pnt = ogr.CreateGeometryFromWkt('POINT(-117.5 32.0)', self.ll_srs)
    self.assertEqual(pnt.Transform(self.ct), 0)

    self.ll_srs = None
    self.ct = None

    self.assertIn('PROJCS', pnt.GetSpatialReference().ExportToPrettyWkt())

  def testCt04SimpleLlUtm(self):
    points = self.ct.TransformPoints([(-117.5, 32., 0.), (-117.5, 32.)])
    expected_point = (452772.0628547681, 3540544.8958501634, 0.)
    for point in points:
      for i in range(3):
        self.assertEqual(point[i], expected_point[i])

    # Test 5 with tuples rather than list of tuples.
    points = self.ct.TransformPoints(((-117.5, 32., 0.), (-117.5, 32.)))
    for point in points:
      for i in range(3):
        self.assertEqual(point[i], expected_point[i])

  def testCt06CreateCoordinateTransformation(self):
    ct = osr.CreateCoordinateTransformation(self.ll_srs, self.utm_srs)
    self.assertIsNotNone(ct)

    points = ct.TransformPoints([(-117.5, 32., 0.), (-117.5, 32.)])
    expected_point = (452772.0628547681, 3540544.8958501634, 0.)
    for point in points:
      for i in range(3):
        self.assertEqual(point[i], expected_point[i])

  def testCtFromAndToEpsg4326(self):
    srs_wgs84 = osr.SpatialReference()
    srs_wgs84.ImportFromEPSG(4326)

    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      for epsg, lon, lat, delta, u, v, delta_inv in TRANSFORM_POINTS:
        srs = osr.SpatialReference()
        ogr_err = srs.ImportFromEPSG(epsg)
        # TODO(schwehr): Use ogr.OGRERR_NONE rather than 0 after GDAL 2 upgrade.
        self.assertEqual(ogr_err, 0, 'Error importing EPSG: %d' % epsg)

        # Test the inverse transformation: projected to WGS84.
        ct_inv = osr.CoordinateTransformation(srs, srs_wgs84)
        xyz_wgs84 = ct_inv.TransformPoint(u, v, 0.0)
        self.assertEqual(gdal.GetLastErrorNo(), 0)  # No error.

        msg = 'lon epsg: %d  delta: %f %f' % (epsg, delta, (xyz_wgs84[0] - lon))
        self.assertAlmostEqual(xyz_wgs84[0], lon, msg=msg, delta=delta)
        msg = 'lat epsg: %d  delta: %f %f' % (epsg, delta, (xyz_wgs84[1] - lat))
        self.assertAlmostEqual(xyz_wgs84[1], lat, msg=msg, delta=delta)

        # Test the forward transformation: WGS84 to projected.
        ct = osr.CoordinateTransformation(srs_wgs84, srs)
        xyz = ct.TransformPoint(*xyz_wgs84)

        msg = 'u epsg: %d  delta_inv: %f %f' % (epsg, delta_inv, (xyz[0] - u))
        self.assertAlmostEqual(xyz[0], u, msg=msg, delta=delta_inv)
        msg = 'v epsg: %d  delta_inv: %f %f' % (epsg, delta_inv, (xyz[1] - v))
        self.assertAlmostEqual(xyz[1], v, msg=msg, delta=delta_inv)


if __name__ == '__main__':
  unittest.main()
