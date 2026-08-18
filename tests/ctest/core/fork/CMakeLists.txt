# Superfície de configuração do fork. Sem GPU, sem VM: só tabela, parsing e camadas.
add_pcsx2_test(fork_config_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../StubHost.cpp
	fork_config_tests.cpp
	gpu_capabilities_tests.cpp
	driver_package_tests.cpp
	driver_identity_tests.cpp
	bridge_tests.cpp
	benchmark_tests.cpp
)

target_include_directories(fork_config_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
)

target_link_libraries(fork_config_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)
