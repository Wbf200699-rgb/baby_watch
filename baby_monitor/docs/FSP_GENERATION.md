# FSP project generation

The CPU0 and CPU1 `configuration.xml` files are the source of truth for FSP-generated content.

Before building in e2 studio:

1. Open `Multicore_top` as an FSP Solution project.
2. Open each CPU project's `configuration.xml`.
3. Click **Generate Project Content** for CPU0 and CPU1.
4. Build `Multicore_top_CPU0` first, then `Multicore_top_CPU1`.

FreeRTOS run-time statistics use the FSP **Custom FreeRTOSConfig.h** setting. It must remain set to `freertos_runtime_stats_config.h`. The referenced headers are source-owned files under each CPU project's `src` directory, so generation must not replace their definitions.

Generated files under `ra_cfg` and `ra_gen` must not be edited by hand. Change `configuration.xml` or a source-owned custom header instead, regenerate, and commit both the configuration and generated output.
