#!/bin/sh
# Builds the three websites into docs/build: the main website and the documentation with Zensical,
# and the API, under api/, with Doxygen. Everything it downloads stays in build/.
set -e
cd "$(dirname "$0")/.."

DOXYGEN_VERSION="1.18.0"
THEME_VERSION="2.5.0"

DOXYGEN_ARCHIVE="doxygen-${DOXYGEN_VERSION}.linux.bin.tar.gz"
DOXYGEN="build/doxygen-${DOXYGEN_VERSION}/bin/doxygen"
THEME_DIR="build/doxygen-awesome-css"
ZENSICAL_ENV="build/website_env"
ZENSICAL="${ZENSICAL_ENV}/bin/zensical"
PYTHON_PACKAGES="docs/requirements.txt"
# A copy of the list the environment was installed from: when the list changes, the environment
# is installed again.
INSTALLED_PYTHON_PACKAGES="${ZENSICAL_ENV}/installed_requirements.txt"

mkdir -p build

if [ ! -x "$DOXYGEN" ]; then
    echo "Downloading Doxygen ${DOXYGEN_VERSION}..."
    DOXYGEN_RELEASE="Release_$(echo "$DOXYGEN_VERSION" | tr . _)"
    wget -q -P build/ "https://github.com/doxygen/doxygen/releases/download/${DOXYGEN_RELEASE}/${DOXYGEN_ARCHIVE}"
    tar -xf "build/${DOXYGEN_ARCHIVE}" -C build/
    rm "build/${DOXYGEN_ARCHIVE}"
fi

if ! grep -q "\"version\": \"${THEME_VERSION}\"" "${THEME_DIR}/package.json" 2>/dev/null; then
    echo "Downloading the Doxygen Awesome theme ${THEME_VERSION}..."
    rm -rf "$THEME_DIR"
    wget -q -P build/ "https://github.com/jothepro/doxygen-awesome-css/archive/refs/tags/v${THEME_VERSION}.zip"
    unzip -q "build/v${THEME_VERSION}.zip" -d build/
    mv "build/doxygen-awesome-css-${THEME_VERSION}" "$THEME_DIR"
    rm "build/v${THEME_VERSION}.zip"
fi

if ! cmp -s "$PYTHON_PACKAGES" "$INSTALLED_PYTHON_PACKAGES"; then
    echo "Installing Zensical and the website's other Python packages..."
    rm -rf "$ZENSICAL_ENV"
    if command -v uv >/dev/null 2>&1; then
        uv venv "$ZENSICAL_ENV"
        uv pip install --python "${ZENSICAL_ENV}/bin/python" -r "$PYTHON_PACKAGES"
    else
        python3 -m venv "$ZENSICAL_ENV"
        "${ZENSICAL_ENV}/bin/pip" install -r "$PYTHON_PACKAGES"
    fi
    cp "$PYTHON_PACKAGES" "$INSTALLED_PYTHON_PACKAGES"
fi

# Doxygen writes the API's pages into Zensical's source folder, which Zensical copies to the output
# unchanged, and the tag file in which the documentation's class links are looked up. So the API
# is built first.
echo "Building the API..."
rm -rf docs/pages/api
"$DOXYGEN" -q docs/doxygen/Doxyfile

echo "Building the main website and the documentation..."
"$ZENSICAL" build --clean --strict -f docs/zensical.toml
echo "The websites are in docs/build."
