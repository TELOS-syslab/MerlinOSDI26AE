./contrib/build.sh -d -j -v #install dependencies and build cachelib.
cd build-cachelib #resulting cachelib files will be stored in the build-cachelib subdirectory.
#Modify source code files in ./cachelib/
make #Rebuild the modified files in build-cachelib using make.
