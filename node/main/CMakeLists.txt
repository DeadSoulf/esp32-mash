set(srcs "main.c"
    "board.c")

idf_component_register(SRCS "automesh_status_led.c" "${srcs}"
                    INCLUDE_DIRS  ".")

