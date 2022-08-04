# This example code is in the Public Domain (or CC0 licensed, at your option.)
# Unless required by applicable law or agreed to in writing, this
# software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, either express or implied.

idf_component_register(
    SRCS
        fakelto.c
        totalsms/rom_database.c

    INCLUDE_DIRS
        .
	include
	totalsms

    EMBED_FILES
        rom.sms
        lcd_controller.bin.gz

    LDFRAGMENTS
        segments.lf 
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Ofast
    -fjump-tables
    -ftree-switch-conversion
    -ftree-loop-im
    -ftree-loop-ivcanon
    -fivopts
    -ftree-vectorize
    -fvariable-expansion-in-unroller
    -Wno-unused-function
)
