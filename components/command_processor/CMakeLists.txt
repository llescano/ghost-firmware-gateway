idf_component_register(
    SRCS "src/command_processor.c"
    INCLUDE_DIRS "include"
    REQUIRES supabase_client controller
)
