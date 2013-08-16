set (RATELIM_SOURCE_FILES
	token_bucket.c
	evratelim.c
)

add_library(libevratelim ${RATELIM_SOURCE_FILES})
set_target_properties(libevratelim PROPERTIES OUTPUT_NAME "evratelim")
