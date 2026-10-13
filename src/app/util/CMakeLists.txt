if (OS_ANDROID)
    set(APP_FONT_SOURCE font_android.c)
else ()
    set(APP_FONT_SOURCE font.c)
endif ()
target_sources(moonlight-lib PRIVATE
        path.c
        img_loader.c
        nullable.c
        ${APP_FONT_SOURCE}
        diag_logs.c diag_report.c crash_log.c)