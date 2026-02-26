# components/controller/CMakeLists.txt
# Componente controlador principal del Gateway

idf_component_register(SRCS "controller.c"
                       INCLUDE_DIRS "include"
                       REQUIRES main ui nvs_flash json supabase_client)
