# components/ui/CMakeLists.txt
# Componente de interfaz de usuario (LED y botón) para el Gateway

idf_component_register(SRCS "ui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES main esp_driver_gpio button)
