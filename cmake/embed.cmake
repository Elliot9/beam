# Turns a file into a C++ byte array (NUL-terminated):
#   cmake -DINPUT=file -DOUTPUT=file.cpp -DSYMBOL=kName -P embed.cmake
file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
get_filename_component(name "${INPUT}" NAME)
file(WRITE "${OUTPUT}" "// Generated from ${name} by cmake/embed.cmake. Do not edit.\n"
                       "extern const unsigned char ${SYMBOL}[];\n"
                       "const unsigned char ${SYMBOL}[] = {\n${bytes}0x00};\n")
