# Investigación canónica de memoria libmpv

La matriz completa y sus artefactos se mantienen en
[video-memory-investigation.md](video-memory-investigation.md). Ese documento
es la única fuente de resultados de esta investigación: contiene la tabla de
hipótesis, el reproducer mínimo, los controles a 60/1 FPS, `report_swap`, y la
lectura causal. Este alias con el nombre solicitado evita duplicar números y
mantiene un único informe editable.

Estado actual: el reproducer mínimo reproduce el crecimiento al llamar
`mpv_render_context_render()` a alta frecuencia, mientras que el callback
update-driven queda prácticamente plano. El cambio de producción sólo se
aceptará después del soak integrado y la validación real.
