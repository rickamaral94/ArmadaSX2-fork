# Fase 2 — métricas da camada de apresentação.
#
# A lógica que importa em GSPresentationMetrics é janela deslizante, separação real/apresentado e
# percentil. Nada disso precisa de GPU, e verificar com o relógio de parede exigiria dormir de
# verdade — teste lento e intermitente para justificar decisões de desempenho é pior que teste
# nenhum. O módulo aceita um relógio injetado, então tudo aqui é determinístico.
add_pcsx2_test(presentation_metrics_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../StubHost.cpp
	presentation_metrics_tests.cpp
)

target_include_directories(presentation_metrics_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../pcsx2
)

target_link_libraries(presentation_metrics_tests PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)
