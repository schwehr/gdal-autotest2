# Description:
#   gdal - a library for reading and writing many raster/vector geospatial files

# NOTES:
#
# This is an incomplete bazel build file.
#
# This counts on copybara to exclude a large fraction of the GDAL
# upstream repo from the local piper repo.
#
# The google3 environment is designed not to enable C++ exceptions.
# There is some weirdness here because GDAL must have exceptions.

gdal_build_copts = [
    "-fexceptions",
]

local_defines_base = [
    # Enable the following flags to help trace issues.
    # TODO(schwehr): Why does DEBUG cause cplkeywordparser_test to fail?
    # "DEBUG",
    # "VSI_DEBUG",
    # "DEBUG_VSIMALLOC",
    "GDAL_COMPILATION",
    "WITHOUT_CPLDEBUG",
]

# For google3/third_party/gdal/port/cpl_compressor.cpp
local_defines_compressors = [
    "HAVE_BLOSC",
    "HAVE_LZ4",
    "HAVE_LIBZ",
    "HAVE_LZMA",
    "HAVE_ZSTD",
]

local_defines_gdal = [
    # "STRICT_OGRERR_TYPE", - Would be nice to enable this.
    # Peristable Auxiliary Metadata (PAM) breaks the sandboxing.
    # "PAM_ENABLED",
    "HAVE_EXPAT",
    "HAVE_GEOS",
    "HAVE_MITAB",
    "GDAL_NO_AUTOLOAD",  # Disabled for security.
    "USE_CPL",
    # Enable additional formats.  See:
    # google3/third_party/gdal/frmts/gdalallregister.cpp
    "FRMT_aaigrid",
    "FRMT_aigrid",
    "FRMT_grib",
    # 1 << 27
    "GRIB_MAX_POINTS=134217728",
    "USE_AEC",
    "FRMT_gtiff",
    "HAVE_TIFF",
    "WEBP_SUPPORT",  # Enable lossless for WebP inside tiffs.
    "HAVE_JXL",  # JPEG XL inside of tiffs.
    "FRMT_hdf4",
    "HAVE_HDF4",
    "HDF4_HAS_MAXOPENFILES",
    "HAVE_HDF5",
    "FRMT_hdf5",
    "FRMT_hfa",
    "FRMT_jp2kak",
    "FRMT_jpeg",
    "FRMT_jpegxl",
    "HAVE_JXL_BOX_API",
    # "HAVE_JxlDecoderDefaultPixelFormat",
    "HAVE_JxlEncoderFrameSettingsCreate",
    "HAVE_JxlEncoderFrameSettingsSetOption",
    "HAVE_JxlEncoderInitExtraChannelInfo",
    "HAVE_JxlEncoderSetCodestreamLevel",
    "HAVE_JxlEncoderSetExtraChannelDistance",
    "HAVE_JxlEncoderSetFrameDistance",
    "HAVE_JxlEncoderSetFrameLossless",
    # "HAVE_JXL_THREADS",
    "HAVE_LIBJPEG",
    "FRMT_mem",
    "FRMT_netcdf",
    "NETCDF_HAS_NC2",
    "HAVE_NETCDF_MEM",
    "FRMT_png",
    "FRMT_raw",
    "FRMT_srtmhgt",
    "FRMT_vrt",
    "FRMT_zarr",
    "GDAL_VRT_DISABLE_PYTHON",
    "FRMT_webp",
    # OGR drivers.  See:
    # google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrregisterall.cpp
    "CSV_ENABLED",
    "GEOJSON_ENABLED",
    "GML_ENABLED",
    "GPKG_ENABLED",  # Raster and vector driver.
    # "SQLITE_HAS_PROGRESS_HANDLER",
    "GPX_ENABLED",
    "KML_ENABLED",
    "MITAB_ENABLED",
    "OPENFILEGDB_ENABLED",
    "OSM_ENABLED",
    "SHAPE_ENABLED",
    "SQLITE_ENABLED",
    "COMPILATION_ALLOWED",  # For SQLITE_ENABLED.
    "HAVE_SQLITE3EXT_H",
    "VRT_ENABLED",
    # TODO(schwehr): Enable C23 file #embed when allowed in google3 - go/cstyle#C++_Version.
    # https://en.cppreference.com/w/c/preprocessor/embed
    # "EMBED_RESOURCE_FILES",
]

# Common porting library from GDAL.  Split out to allow libgeotiff to use
# the VSI file system layer and to make some of the tests smaller and faster.
cc_library(
    name = "port_lib",
    srcs = glob(
        [
            "port/cpl*.cpp",
        ],
        exclude = [
            "port/cpl_character_sets.c",
            "port/cpl_json.cpp",
        ],
    ) + [
        "google/gdal_init.cc",
        "google/gdal_init.h",
    ],
    hdrs = glob([
        "gcore/*.h",
        "ogr/*.h",
        "port/*.h",
        "port/*.hpp",
    ]),
    copts = gdal_build_copts + [
        "-Wno-implicit-fallthrough",
    ],
    defines = ["ALLOW_DEPRECATED_CPL_PATH_FUNCTIONS"],
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    includes = [
        "gcore",
        "ogr",
        "port",
    ],
    linkopts = [
    ] + select({
        "//tools/cc_target_os:android": [],
        "//conditions:default": [
            "-lpthread",
        ],
    }),
    local_defines = local_defines_base + local_defines_compressors,
    textual_hdrs = [
        "port/cpl_character_sets.c",
    ],
    deps = [
        ":gdal_embed_data",
        "//third_party/absl/base",
        "//third_party/blosc",
        "//third_party/json_c",
        "//third_party/liblzma",
        "//third_party/lz4",
        "//third_party/stl",
        "//third_party/zlib",
        "//third_party/zstdlib",
    ],
)

cc_library(
    name = "gdal",
    srcs = glob(
        [
            "alg/*",
            "alg/viewshed/*",
            "apps/gdalalg_main.*",
            "apps/argparse/argparse.*",
            "apps/commonutils.*",
            "apps/*lib*.cpp",
            "frmts/**",
            "gcore/**",
            "ogr/**",
        ] + ["apps/gdalargumentparser.cpp"],
        exclude = [
            "alg/polygonize_polygonizer.cpp",
            "frmts/png/filter_sse2_intrinsics.c",
            "ogr/ogrsf_frmts/geojson/libjson/*",
            "ogr/ogrsf_frmts/shape/dbfopen.c",
            "ogr/ogrsf_frmts/shape/sbnsearch.c",
            "ogr/ogrsf_frmts/shape/shpopen.c",
            "ogr/ogrsf_frmts/shape/shptree.c",
            "ogr/ogrsf_frmts/sqlite/ogrsqliteregexp.cpp",
            "ogr/ogrsf_frmts/sqlite/ogrsqlitesqlfunctions.cpp",
            "ogr/ogrsf_frmts/sqlite/ogrsqlitesqlfunctionscommon.cpp",
            "ogr/ogrsf_frmts/sqlite/sqlite_rtree_bulk_load/sqlite_rtree_bulk_load.c",
        ],
    ) + [
        "port/cpl_json.cpp",
    ],
    hdrs = glob(["**/*.h"]),
    copts = gdal_build_copts + [
        "-Wno-macro-redefined",
        "-Wno-unused-variable",
    ],
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    includes = [
        "alg",
        "alg/marching_squares",
        "apps",
        "apps/argparse",
        "frmts",
        "frmts/grib",
        "frmts/grib/degrib/degrib",
        "frmts/grib/degrib/g2clib",
        "frmts/gtiff",
        "frmts/hdf4",
        "frmts/hdf4/hdf-eos",
        "frmts/jpeg",
        "frmts/mem",
        "frmts/netcdf",
        "frmts/pds",
        "frmts/raw",
        "frmts/vrt",
        "gcore",
        "ogr",
        "ogr/ogrsf_frmts",
        "ogr/ogrsf_frmts/avc",
        "ogr/ogrsf_frmts/generic",
        "ogr/ogrsf_frmts/geojson",
        "ogr/ogrsf_frmts/gmlutils",
        "ogr/ogrsf_frmts/mem",
        "ogr/ogrsf_frmts/mitab",
        "ogr/ogrsf_frmts/shape",
        "ogr/ogrsf_frmts/sqlite",
        "ogr/ogrsf_frmts/vrt",
        "port",
    ],
    linkopts = [
        "-lpthread",
    ],
    # Only need blosc from the local_defines_compressors for zarr.
    local_defines = local_defines_base + local_defines_gdal + local_defines_compressors,
    textual_hdrs = [
        "alg/polygonize_polygonizer.cpp",
        "frmts/png/filter_sse2_intrinsics.c",
        "ogr/ogrsf_frmts/shape/dbfopen.c",
        "ogr/ogrsf_frmts/shape/sbnsearch.c",
        "ogr/ogrsf_frmts/shape/shpopen.c",
        "ogr/ogrsf_frmts/shape/shptree.c",
        "ogr/ogrsf_frmts/sqlite/ogrsqliteregexp.cpp",
        "ogr/ogrsf_frmts/sqlite/ogrsqlitesqlfunctions.cpp",
        "ogr/ogrsf_frmts/sqlite/ogrsqlitesqlfunctionscommon.cpp",
        "ogr/ogrsf_frmts/sqlite/sqlite_rtree_bulk_load/sqlite_rtree_bulk_load.c",
    ],
    deps = [
        ":gdal_embed_data",
        ":port_lib",
        "//third_party/cpp/expat",
        "//third_party/geos",
        "//third_party/hdf4",
        "//third_party/hdf5",
        "//third_party/jpegxl",
        "//third_party/json_c",
        "//third_party/kakadu:kdutools",
        "//third_party/libaec",
        "//third_party/libgeotiff",
        "//third_party/libjpeg_turbo/src:jpeg",
        "//third_party/libwebp:webp",
        "//third_party/netcdf",
        "//third_party/png",
        "//third_party/proj4",
        "//third_party/sqlite",
        "//third_party/tiff",
    ],
)

cc_binary(
    name = "ogr2ogr",
    srcs = ["apps/ogr2ogr_bin.cpp"],
    copts = gdal_build_copts,
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    local_defines = local_defines_base,
    deps = [":gdal"],
)

cc_binary(
    name = "ogrinfo",
    srcs = ["apps/ogrinfo_bin.cpp"],
    copts = gdal_build_copts,
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    local_defines = local_defines_base,
    deps = [":gdal"],
)

cc_binary(
    name = "gdalsrsinfo",
    srcs = ["apps/gdalsrsinfo.cpp"],
    copts = gdal_build_copts,
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    local_defines = local_defines_base,
    deps = [":gdal"],
)

cc_binary(
    name = "gdaltransform",
    srcs = ["apps/gdaltransform.cpp"],
    copts = gdal_build_copts,
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    local_defines = local_defines_base,
    deps = [":gdal"],
)

cc_binary(
    name = "gdal_rasterize",
    srcs = ["apps/gdal_rasterize_bin.cpp"],
    copts = gdal_build_copts,
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    local_defines = local_defines_base,
    deps = [":gdal"],
)

# A filegroup target containing all of the gdal data files for embedding.
filegroup(
    name = "gdal_data_fg",
    srcs = glob(include = ["data/*"]),
    licenses = ["notice"],
)

cc_embed_data(
    name = "gdal_embed_data",
    srcs = [":gdal_data_fg"],
    outs = [
        "gdal_data.cc",
        "gdal_data.h",
        "gdal_data_data.o",
    ],
    flatten = 1,
)

cc_binary(
    name = "gdalinfo",
    srcs = ["apps/gdalinfo_bin.cpp"],
    copts = gdal_build_copts,
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    local_defines = local_defines_base,
    deps = [":gdal"],
)

cc_binary(
    name = "gdaladdo",
    srcs = ["apps/gdaladdo.cpp"],
    copts = gdal_build_copts,
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    local_defines = local_defines_base + ["GTI_DRIVER_DISABLED_OR_PLUGIN"],
    deps = [":gdal"],
)

cc_binary(
    name = "gdal_translate",
    srcs = ["apps/gdal_translate_bin.cpp"],
    copts = gdal_build_copts,
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    local_defines = local_defines_base,
    deps = [":gdal"],
)

cc_binary(
    name = "gdalwarp",
    srcs = ["apps/gdalwarp_bin.cpp"],
    copts = gdal_build_copts,
    features = ["-use_header_modules"],  # Incompatible with -fexceptions.
    local_defines = local_defines_base,
    deps = [":gdal"],
)

py_library(
    name = "py_gdal",
    srcs = [
        "swig/python/osgeo/gdal.py",
        "swig/python/osgeo/gdal_array.py",
        "swig/python/osgeo/gdalnumeric.py",
    ],
    deps = [
        ":gdal.so",
        ":gdal_array.so",
        ":py_gdalconst",
        "//third_party/py/numpy",
    ],
)

py_library(
    name = "py_gdalconst",
    srcs = ["swig/python/osgeo/gdalconst.py"],
    deps = [":gdalconst.so"],
)

py_library(
    name = "py_ogr",
    srcs = ["swig/python/osgeo/ogr.py"],
    deps = [
        ":ogr.so",
        ":py_osr",
    ],
)

py_library(
    name = "py_osr",
    srcs = ["swig/python/osgeo/osr.py"],
    deps = [":osr.so"],
)

py_binary(
    name = "gdal_merge",
    srcs = ["swig/python/osgeo/utils/gdal_merge.py"],
    deps = [
        "//third_party/py/numpy",
        "//third_party/py/osgeo",
    ],
)

py_binary(
    name = "gdal2tiles",
    srcs = ["swig/python/scripts/gdal2tiles.py"],
    deps = [
        "//third_party/py/PIL:pil",
        "//third_party/py/osgeo",
    ],
)

py_library(
    name = "validate_cloud_optimized_geotiff_lib",
    srcs = ["swig/python/samples/validate_cloud_optimized_geotiff.py"],
    deps = ["//third_party/py/osgeo"],
)

py_binary(
    name = "validate_cloud_optimized_geotiff",
    testonly = 1,
    srcs = ["swig/python/samples/validate_cloud_optimized_geotiff.py"],
    deps = [
        ":validate_cloud_optimized_geotiff_lib",
    ],
)

py_library(
    name = "validate_gpkg_lib",
    srcs = ["swig/python/gdal-utils/osgeo_utils/samples/validate_gpkg.py"],
    deps = [
        "//third_party/py/osgeo",
        "//third_party/py/sqlite3",
    ],
)

py_binary(
    name = "validate_gpkg",
    testonly = 1,
    srcs = ["swig/python/gdal-utils/osgeo_utils/samples/validate_gpkg.py"],
    deps = [
        ":validate_gpkg_lib",
    ],
)

py_extension(
    name = "gdal.so",
    srcs = [
        "swig/python/extensions/gdal_wrap.cpp",
    ],
    copts = [
        "-Wno-implicit-fallthrough",
    ],
    local_defines = local_defines_base,
    module_name = "swig.python.osgeo._gdal",
    deps = [
        ":gdal",
        "//third_party/py/numpy:headers",
        "//third_party/python_runtime:headers",
    ],
)

py_extension(
    name = "gdal_array.so",
    srcs = ["swig/python/extensions/gdal_array_wrap.cpp"],
    local_defines = local_defines_base,
    module_name = "swig.python.osgeo._gdal_array",
    deps = [
        ":gdal",
        "//third_party/py/numpy:headers",
        "//third_party/python_runtime:headers",
    ],
)

py_extension(
    name = "gdalconst.so",
    srcs = [
        "swig/python/extensions/gdalconst_wrap.c",
    ],
    local_defines = local_defines_base,
    module_name = "swig.python.osgeo._gdalconst",
    deps = [
        ":gdal",
        "//third_party/py/numpy:headers",
        "//third_party/python_runtime:headers",
    ],
)

py_extension(
    name = "ogr.so",
    srcs = [
        "swig/python/extensions/ogr_wrap.cpp",
    ],
    local_defines = local_defines_base,
    module_name = "swig.python.osgeo._ogr",
    deps = [
        ":gdal",
        "//third_party/py/numpy:headers",
        "//third_party/python_runtime:headers",
    ],
)

py_extension(
    name = "osr.so",
    srcs = [
        "swig/python/extensions/osr_wrap.cpp",
    ],
    local_defines = local_defines_base,
    module_name = "swig.python.osgeo._osr",
    deps = [
        ":gdal",
        "//third_party/py/numpy:headers",
        "//third_party/python_runtime:headers",
    ],
)
