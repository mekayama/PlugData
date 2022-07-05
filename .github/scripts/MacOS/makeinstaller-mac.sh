#!/bin/bash

# PlugData macOS installer build script, using pkgbuild and productbuild
# based on script for SURGE https://github.com/surge-synthesizer/surge and iPlug2 https://github.com/iPlug2/iPlug2

# Documentation for pkgbuild and productbuild: https://developer.apple.com/library/archive/documentation/DeveloperTools/Reference/DistributionDefinitionRef/Chapters/Distribution_XML_Ref.html


# version
if [ "$PLUGIN_VERSION" != "" ]; then
  VERSION="$PLUGIN_VERSION"
elif [ "$1" != "" ]; then
  VERSION="$1"
fi

if [ "$VERSION" == "" ]; then
  echo "You must specify the version you are packaging as the first argument!"
  echo "eg: makeinstaller-mac.sh 0.6.0"
  exit 1
fi

PRODUCT_NAME=PlugData

# locations
PRODUCTS="Plugins"

LV2="/LV2/."
VST3="/VST3/."
AU="/AU/."
APP="/Standalone/."

OUTPUT_BASE_FILENAME="${PRODUCT_NAME}.pkg"

TARGET_DIR="./"
PKG_DIR=${TARGET_DIR}/pkgs

if [[ ! -d ${TARGET_DIR} ]]; then
  mkdir ${TARGET_DIR}
fi

if [[ ! -d ${PKG_DIR} ]]; then
  mkdir ${PKG_DIR}
fi

build_flavor()
{
  TMPDIR=${TARGET_DIR}/tmp
  flavor=$1
  flavorprod=$2
  ident=$3
  loc=$4

  echo --- BUILDING ${PRODUCT_NAME}_${flavor}.pkg ---

  mkdir -p $TMPDIR
  cp -a $PRODUCTS/$flavorprod $TMPDIR

  pkgbuild --root $TMPDIR --identifier $ident --version $VERSION --install-location $loc ${PKG_DIR}/${PRODUCT_NAME}_${flavor}.pkg #|| exit 1

  rm -r $TMPDIR
}

# try to build LV2 package
if [[ -d $PRODUCTS/$LV2 ]]; then
  build_flavor "LV2" $LV2 "com.Octagon.PlugData.lv2.pkg.${PRODUCT_NAME}" "/Library/Audio/Plug-Ins/LV2"
fi

# # try to build VST3 package
if [[ -d $PRODUCTS/$VST3 ]]; then
  build_flavor "VST3" $VST3 "com.Octagon.PlugData.vst3.pkg.${PRODUCT_NAME}" "/Library/Audio/Plug-Ins/VST3"
fi

# # try to build AU package
if [[ -d $PRODUCTS/$AU ]]; then
  build_flavor "AU" $AU "com.Octagon.PlugData.au.pkg.${PRODUCT_NAME}" "/Library/Audio/Plug-Ins/Components"
fi

# try to build App package
if [[ -d $PRODUCTS/$APP ]]; then
  build_flavor "APP" $APP "com.Octagon.PlugData.app.pkg.${PRODUCT_NAME}" "/Applications"
fi


if [[ -d $PRODUCTS/$VST3 ]]; then
	VST3_PKG_REF="<pkg-ref id=\"com.Octagon.PlugData.vst3.pkg.${PRODUCT_NAME}\"/>"
	VST3_CHOICE="<line choice=\"com.Octagon.PlugData.vst3.pkg.${PRODUCT_NAME}\"/>"
	VST3_CHOICE_DEF="<choice id=\"com.Octagon.PlugData.vst3.pkg.${PRODUCT_NAME}\" visible=\"true\" start_selected=\"true\" title=\"VST3 Plug-in\"><pkg-ref id=\"com.Octagon.PlugData.vst3.pkg.${PRODUCT_NAME}\"/></choice><pkg-ref id=\"com.Octagon.PlugData.vst3.pkg.${PRODUCT_NAME}\" version=\"${VERSION}\" onConclusion=\"none\">${PRODUCT_NAME}_VST3.pkg</pkg-ref>"
fi
if [[ -d $PRODUCTS/$LV2 ]]; then
	LV2_PKG_REF="<pkg-ref id=\"com.Octagon.PlugData.lv2.pkg.${PRODUCT_NAME}\"/>"
	LV2_CHOICE="<line choice=\"com.Octagon.PlugData.lv2.pkg.${PRODUCT_NAME}\"/>"
	LV2_CHOICE_DEF="<choice id=\"com.Octagon.PlugData.lv2.pkg.${PRODUCT_NAME}\" visible=\"true\" start_selected=\"true\" title=\"LV2 Plug-in\"><pkg-ref id=\"com.Octagon.PlugData.lv2.pkg.${PRODUCT_NAME}\"/></choice><pkg-ref id=\"com.Octagon.PlugData.lv2.pkg.${PRODUCT_NAME}\" version=\"${VERSION}\" onConclusion=\"none\">${PRODUCT_NAME}_LV2.pkg</pkg-ref>"
fi
if [[ -d $PRODUCTS/$AU ]]; then
	AU_PKG_REF="<pkg-ref id=\"com.Octagon.PlugData.au.pkg.${PRODUCT_NAME}\"/>"
	AU_CHOICE="<line choice=\"com.Octagon.PlugData.au.pkg.${PRODUCT_NAME}\"/>"
	AU_CHOICE_DEF="<choice id=\"com.Octagon.PlugData.au.pkg.${PRODUCT_NAME}\" visible=\"true\" start_selected=\"true\" title=\"Audio Unit Plug-in\"><pkg-ref id=\"com.Octagon.PlugData.au.pkg.${PRODUCT_NAME}\"/></choice><pkg-ref id=\"com.Octagon.PlugData.au.pkg.${PRODUCT_NAME}\" version=\"${VERSION}\" onConclusion=\"none\">${PRODUCT_NAME}_AU.pkg</pkg-ref>"
fi
if [[ -d $PRODUCTS/$APP ]]; then
	APP_PKG_REF="<pkg-ref id=\"com.Octagon.PlugData.app.pkg.${PRODUCT_NAME}\"/>"
	APP_CHOICE="<line choice=\"com.Octagon.PlugData.app.pkg.${PRODUCT_NAME}\"/>"
	APP_CHOICE_DEF="<choice id=\"com.Octagon.PlugData.app.pkg.${PRODUCT_NAME}\" visible=\"true\" start_selected=\"true\" title=\"Standalone App\"><pkg-ref id=\"com.Octagon.PlugData.app.pkg.${PRODUCT_NAME}\"/></choice><pkg-ref id=\"com.Octagon.PlugData.app.pkg.${PRODUCT_NAME}\" version=\"${VERSION}\" onConclusion=\"none\">${PRODUCT_NAME}_APP.pkg</pkg-ref>"
fi


touch ${TARGET_DIR}/distribution.xml
cat > ${TARGET_DIR}/distribution.xml << XMLEND
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>${PRODUCT_NAME} ${VERSION}</title>
    <license file="LICENSE.rtf" mime-type="application/rtf"/>
    ${VST3_PKG_REF}
    ${AU_PKG_REF}
    ${LV2_PKG_REF}
    ${APP_PKG_REF}
    <options require-scripts="false" customize="always" />
    <choices-outline>
        ${VST3_CHOICE}
        ${AU_CHOICE}
        ${LV2_CHOICE}
        ${APP_CHOICE}
    </choices-outline>
    ${VST3_CHOICE_DEF}
    ${AU_CHOICE_DEF}
    ${LV2_CHOICE_DEF}
    ${APP_CHOICE_DEF}
</installer-gui-script>
XMLEND

# build installation bundle
# --resources .

productbuild --distribution ${TARGET_DIR}/distribution.xml --package-path ${PKG_DIR} "${TARGET_DIR}/$OUTPUT_BASE_FILENAME"

rm ${TARGET_DIR}/distribution.xml
rm -r $PKG_DIR


productsign -f -s "Developer ID Installer: Timothy Schoen (7SV7JPRR2L)" ${PRODUCT_NAME}.pkg ${PRODUCT_NAME}-MacOS-Universal.pkg
