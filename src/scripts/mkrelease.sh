#!/bin/bash
if [ -z $QT_STATIC ]; then 
    echo "QT_STATIC is not set. Please set it to the base directory of a statically compiled Qt"; 
    exit 1; 
fi

if [ -z $APP_VERSION ]; then echo "APP_VERSION is not set"; exit 1; fi
if [ -z $PREV_VERSION ]; then echo "PREV_VERSION is not set"; exit 1; fi

if [ -z $MOONROOMCASH_DIR ]; then
    echo "MOONROOMCASH_DIR is not set. Please set it to the base directory of a Moonroomcash project with built Moonroomcash binaries."
    exit 1;
fi

if [ ! -f $MOONROOMCASH_DIR/artifacts/moonroomcashd ]; then
    echo "Couldn't find moonroomcashd in $MOONROOMCASH_DIR/artifacts/. Please build moonroomcashd."
    exit 1;
fi

if [ ! -f $MOONROOMCASH_DIR/artifacts/moonroomcash-cli ]; then
    echo "Couldn't find moonroomcash-cli in $MOONROOMCASH_DIR/artifacts/. Please build moonroomcashd."
    exit 1;
fi

# Ensure that moonroomcashd is the right build
echo -n "moonroomcashd version........."
if grep -q "MagicBean" $MOONROOMCASH_DIR/artifacts/moonroomcashd && ! readelf -s $MOONROOMCASH_DIR/artifacts/moonroomcashd | grep -q "GLIBC_2\.25"; then 
    echo "[OK]"
else
    echo "[ERROR]"
    echo "moonroomcashd doesn't seem to be a MagicBean build or moonroomcashd is built with libc 2.25"
    exit 1
fi

echo -n "moonroomcashd.exe version....."
if grep -q "MagicBean" $MOONROOMCASH_DIR/artifacts/moonroomcashd.exe; then 
    echo "[OK]"
else
    echo "[ERROR]"
    echo "moonroomcashd doesn't seem to be a MagicBean build"
    exit 1
fi

echo -n "Version files.........."
# Replace the version number in the .pro file so it gets picked up everywhere
sed -i "s/${PREV_VERSION}/${APP_VERSION}/g" mrc-qt-wallet.pro > /dev/null

# Also update it in the README.md
sed -i "s/${PREV_VERSION}/${APP_VERSION}/g" README.md > /dev/null
echo "[OK]"

echo -n "Cleaning..............."
rm -rf bin/*
rm -rf artifacts/*
make distclean >/dev/null 2>&1
echo "[OK]"

echo ""
echo "[Building on" `lsb_release -r`"]"

echo -n "Configuring............"
$QT_STATIC/bin/qmake mrc-qt-wallet.pro -spec linux-clang CONFIG+=release > /dev/null
#Mingw seems to have trouble with precompiled headers, so strip that option from the .pro file
echo "[OK]"


echo -n "Building..............."
rm -rf bin/mrc-qt-wallet* > /dev/null
make -j$(nproc) > /dev/null
echo "[OK]"


# Test for Qt
echo -n "Static link............"
if [[ $(ldd mrc-qt-wallet | grep -i "Qt") ]]; then
    echo "FOUND QT; ABORT"; 
    exit 1
fi
echo "[OK]"


echo -n "Packaging.............."
mkdir bin/mrc-qt-wallet-v$APP_VERSION > /dev/null
strip mrc-qt-wallet
cp mrc-qt-wallet bin/mrc-qt-wallet-v$APP_VERSION > /dev/null
cp $MOONROOMCASH_DIR/artifacts/moonroomcashd bin/mrc-qt-wallet-v$APP_VERSION > /dev/null
cp $MOONROOMCASH_DIR/artifacts/moonroomcash-cli bin/mrc-qt-wallet-v$APP_VERSION > /dev/null
cp README.md bin/mrc-qt-wallet-v$APP_VERSION > /dev/null
cp LICENSE bin/mrc-qt-wallet-v$APP_VERSION > /dev/null
cd bin && tar cvf linux-mrc-qt-wallet-v$APP_VERSION.tar.gz mrc-qt-wallet-v$APP_VERSION/ > /dev/null
cd .. 
mkdir artifacts >/dev/null 2>&1
cp bin/linux-mrc-qt-wallet-v$APP_VERSION.tar.gz ./artifacts
echo "[OK]"


if [ -f artifacts/linux-mrc-qt-wallet-v$APP_VERSION.tar.gz ] ; then
    echo -n "Package contents......."
    # Test if the package is built OK
    if tar tf "artifacts/linux-mrc-qt-wallet-v$APP_VERSION.tar.gz" | wc -l | grep -q "6"; then 
        echo "[OK]"
    else
        echo "[ERROR]"
        exit 1
    fi    
else
    echo "[ERROR]"
    exit 1
fi

echo -n "Building deb..........."
debdir=bin/deb/mrc-qt-wallet-v$APP_VERSION
mkdir -p $debdir > /dev/null
mkdir $debdir/DEBIAN
mkdir -p $debdir/usr/local/bin

cat src/scripts/control | sed "s/RELEASE_VERSION/$APP_VERSION/g" > $debdir/DEBIAN/control

cp mrc-qt-wallet $debdir/usr/local/bin/
cp $MOONROOMCASH_DIR/artifacts/moonroomcashd $debdir/usr/local/bin/zqw-moonroomcashd

mkdir -p $debdir/usr/share/pixmaps/
cp res/mrc-qt-wallet.xpm $debdir/usr/share/pixmaps/

mkdir -p $debdir/usr/share/applications
cp src/scripts/desktopentry $debdir/usr/share/applications/mrc-qt-wallet.desktop

dpkg-deb --build $debdir >/dev/null
cp $debdir.deb artifacts/
echo "[OK]"



echo ""
echo "[Windows]"

if [ -z $MXE_PATH ]; then 
    echo "MXE_PATH is not set. Set it to ~/github/mxe/usr/bin if you want to build Windows"
    echo "Not building Windows"
    exit 0; 
fi

if [ ! -f $MOONROOMCASH_DIR/artifacts/moonroomcashd.exe ]; then
    echo "Couldn't find moonroomcashd.exe in $MOONROOMCASH_DIR/artifacts/. Please build moonroomcashd.exe"
    exit 1;
fi


if [ ! -f $MOONROOMCASH_DIR/artifacts/moonroomcash-cli.exe ]; then
    echo "Couldn't find moonroomcash-cli.exe in $MOONROOMCASH_DIR/artifacts/. Please build moonroomcashd.exe"
    exit 1;
fi

export PATH=$MXE_PATH:$PATH

echo -n "Configuring............"
make clean  > /dev/null
rm -f mrc-qt-wallet-mingw.pro
rm -rf release/
#Mingw seems to have trouble with precompiled headers, so strip that option from the .pro file
cat mrc-qt-wallet.pro | sed "s/precompile_header/release/g" | sed "s/PRECOMPILED_HEADER.*//g" > mrc-qt-wallet-mingw.pro
echo "[OK]"


echo -n "Building..............."
x86_64-w64-mingw32.static-qmake-qt5 mrc-qt-wallet-mingw.pro CONFIG+=release > /dev/null
make -j32 > /dev/null
echo "[OK]"


echo -n "Packaging.............."
mkdir release/mrc-qt-wallet-v$APP_VERSION  
cp release/mrc-qt-wallet.exe release/mrc-qt-wallet-v$APP_VERSION 
cp $MOONROOMCASH_DIR/artifacts/moonroomcashd.exe release/mrc-qt-wallet-v$APP_VERSION > /dev/null
cp $MOONROOMCASH_DIR/artifacts/moonroomcash-cli.exe release/mrc-qt-wallet-v$APP_VERSION > /dev/null
cp README.md release/mrc-qt-wallet-v$APP_VERSION 
cp LICENSE release/mrc-qt-wallet-v$APP_VERSION 
cd release && zip -r Windows-mrc-qt-wallet-v$APP_VERSION.zip mrc-qt-wallet-v$APP_VERSION/ > /dev/null
cd ..
mkdir artifacts >/dev/null 2>&1
cp release/Windows-mrc-qt-wallet-v$APP_VERSION.zip ./artifacts/
echo "[OK]"

if [ -f artifacts/Windows-mrc-qt-wallet-v$APP_VERSION.zip ] ; then
    echo -n "Package contents......."
    if unzip -l "artifacts/Windows-mrc-qt-wallet-v$APP_VERSION.zip" | wc -l | grep -q "11"; then 
        echo "[OK]"
    else
        echo "[ERROR]"
        exit 1
    fi
else
    echo "[ERROR]"
    exit 1
fi
