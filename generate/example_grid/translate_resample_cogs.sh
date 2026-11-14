#!/usr/bin/env bash
#
# Generates Cloud-Optimized GeoTIFFs (COGs) for all 9 GDAL overview resampling methods:
# NEAREST, AVERAGE, BILINEAR, CUBIC, CUBICSPLINE, LANCZOS, MODE, RMS, GAUSS.
#
# Each resulting raster is shifted to the east by a configurable step (default: 80 km)
# so they render side-by-side on a map without overlapping.
# An optional flag translates the results north by 200 km.
#

set -euo pipefail

usage() {
  local exit_code="${1:-0}"
  cat <<EOF
Usage: $(basename "$0") [options] [input_file]

Runs gdal_translate to generate COGs across all 9 overview resampling methods,
shifting each subsequent raster to the east so they render side-by-side.

Arguments:
  input_file               Input GeoTIFF (default: ./resample_grid_int16.tif)

Options:
  -s, --step-km <num>      Shift distance between adjacent COGs in km (default: 80)
  -o, --start-offset-km <num>
                           Initial shift offset for the first COG in km (default: 80)
                           Set to 80 to shift COG #1 east of the original input raster;
                           set to 0 to place COG #1 at the input raster's origin.
  -N, --north              Translate the results north by 200 km
  -d, --output-dir <dir>   Output directory (default: .)
  -n, --dry-run            Print commands without executing them
  -h, --help               Show this help message

Resampling methods:
  NEAREST, AVERAGE, BILINEAR, CUBIC, CUBICSPLINE, LANCZOS, MODE, RMS, GAUSS

Creation options:
  -of COG -co COMPRESS=ZSTD -co LEVEL=22 -co NUM_THREADS=ALL_CPUS -co BIGTIFF=YES -co PREDICTOR=YES
EOF
  exit "${exit_code}"
}

# Default parameters
INPUT_FILE="./resample_grid_int16.tif"
SHIFT_STEP_KM=80
START_OFFSET_KM=80
NORTH_SHIFT_KM=200
TRANSLATE_NORTH=false
OUTPUT_DIR="."
DRY_RUN=false

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    -s|--step-km)
      SHIFT_STEP_KM="$2"
      shift 2
      ;;
    -o|--start-offset-km)
      START_OFFSET_KM="$2"
      shift 2
      ;;
    -N|--north|--translate-north)
      TRANSLATE_NORTH=true
      shift
      ;;
    -d|--output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    -n|--dry-run)
      DRY_RUN=true
      shift
      ;;
    -h|--help)
      usage
      ;;
    -*)
      echo "Error: Unknown option '$1'" >&2
      usage
      ;;
    *)
      INPUT_FILE="$1"
      shift
      ;;
  esac
done

# Validate prerequisites
if ! command -v gdal_translate &>/dev/null; then
  echo "Error: gdal_translate not found in PATH." >&2
  exit 1
fi

if ! command -v gdalinfo &>/dev/null; then
  echo "Error: gdalinfo not found in PATH." >&2
  exit 1
fi

if [[ ! -f "${INPUT_FILE}" ]]; then
  echo "Error: Input file '${INPUT_FILE}' does not exist." >&2
  exit 1
fi

mkdir -p "${OUTPUT_DIR}"

# Extract bounding box from input raster
read -r ORIG_ULX ORIG_ULY ORIG_LRX ORIG_LRY < <(python3 -c '
import sys, json, subprocess
try:
    out = subprocess.check_output(["gdalinfo", "-json", sys.argv[1]], text=True)
    info = json.loads(out)
    ul = info["cornerCoordinates"]["upperLeft"]
    lr = info["cornerCoordinates"]["lowerRight"]
    print(f"{ul[0]} {ul[1]} {lr[0]} {lr[1]}")
except Exception as e:
    sys.stderr.write(f"Failed to read raster metadata: {e}\n")
    sys.exit(1)
' "${INPUT_FILE}")

INPUT_BASENAME="$(basename "${INPUT_FILE}")"
INPUT_NAME="${INPUT_BASENAME%.*}"

METHODS=(
  "NEAREST"
  "AVERAGE"
  "BILINEAR"
  "CUBIC"
  "CUBICSPLINE"
  "LANCZOS"
  "MODE"
  "RMS"
  "GAUSS"
)

echo "================================================================================"
echo "GDAL COG Overview Resampling Batch Generator"
echo "================================================================================"
echo "Input file:        ${INPUT_FILE}"
echo "Original bounds:   UL=(${ORIG_ULX}, ${ORIG_ULY})  LR=(${ORIG_LRX}, ${ORIG_LRY})"
echo "Shift step:        ${SHIFT_STEP_KM} km ($((SHIFT_STEP_KM * 1000)) meters)"
echo "Start offset:      ${START_OFFSET_KM} km ($((START_OFFSET_KM * 1000)) meters)"
if [[ "${TRANSLATE_NORTH}" == "true" ]]; then
  echo "Translate north:   true (${NORTH_SHIFT_KM} km / $((NORTH_SHIFT_KM * 1000)) meters)"
else
  echo "Translate north:   false"
fi
echo "Output directory:  ${OUTPUT_DIR}"
echo "Resampling count:  ${#METHODS[@]} methods"
echo "Dry run:           ${DRY_RUN}"
echo "================================================================================"

if [[ "${TRANSLATE_NORTH}" == "true" ]]; then
  NORTH_SHIFT_M=$((NORTH_SHIFT_KM * 1000))
  read -r NEW_ULY NEW_LRY < <(python3 -c '
import sys
uly = float(sys.argv[1])
lry = float(sys.argv[2])
shift = float(sys.argv[3])
print(f"{uly + shift:.3f} {lry + shift:.3f}")
' "${ORIG_ULY}" "${ORIG_LRY}" "${NORTH_SHIFT_M}")
else
  NEW_ULY="${ORIG_ULY}"
  NEW_LRY="${ORIG_LRY}"
fi

for i in "${!METHODS[@]}"; do
  METHOD="${METHODS[$i]}"
  OUTPUT_FILE="${OUTPUT_DIR}/${INPUT_NAME}_${METHOD}.tif"

  # Calculate shift in meters: START_OFFSET_KM + (i * SHIFT_STEP_KM)
  SHIFT_M=$(python3 -c "print((${START_OFFSET_KM} + ${i} * ${SHIFT_STEP_KM}) * 1000)")

  # Calculate shifted bounding coordinates
  read -r NEW_ULX NEW_LRX < <(python3 -c '
import sys
ulx = float(sys.argv[1])
lrx = float(sys.argv[2])
shift = float(sys.argv[3])
print(f"{ulx + shift:.3f} {lrx + shift:.3f}")
' "${ORIG_ULX}" "${ORIG_LRX}" "${SHIFT_M}")

  echo ""
  echo "[$((i + 1))/${#METHODS[@]}] Generating COG with OVERVIEW_RESAMPLING=${METHOD}"
  echo "       Shift east:  ${SHIFT_M} m ($((SHIFT_M / 1000)) km)"
  if [[ "${TRANSLATE_NORTH}" == "true" ]]; then
    echo "       Shift north: ${NORTH_SHIFT_M} m (${NORTH_SHIFT_KM} km)"
  fi
  echo "       New bounds:  UL=(${NEW_ULX}, ${NEW_ULY})  LR=(${NEW_LRX}, ${NEW_LRY})"
  echo "       Output file: ${OUTPUT_FILE}"

  CMD=(
    gdal_translate
    -of COG
    -co COMPRESS=ZSTD
    -co LEVEL=22
    -co NUM_THREADS=ALL_CPUS
    -co BIGTIFF=YES
    -co PREDICTOR=YES
    -co "OVERVIEW_RESAMPLING=${METHOD}"
    -a_ullr "${NEW_ULX}" "${NEW_ULY}" "${NEW_LRX}" "${NEW_LRY}"
    "${INPUT_FILE}"
    "${OUTPUT_FILE}"
  )

  if [[ "${DRY_RUN}" == "true" ]]; then
    printf '       Command:     '
    printf '%q ' "${CMD[@]}"
    printf '\n'
  else
    START_TIME=$(date +%s)
    "${CMD[@]}"
    END_TIME=$(date +%s)
    ELAPSED=$((END_TIME - START_TIME))
    FILE_SIZE=$(du -h "${OUTPUT_FILE}" | cut -f1)
    echo "       Completed in ${ELAPSED}s (Size: ${FILE_SIZE})"
  fi
done

echo ""
echo "================================================================================"
echo "All tasks finished successfully."
echo "================================================================================"
