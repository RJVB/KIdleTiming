# set(osx_plugin_SRCS
#     macpoller.cpp
#     macpoller_helper.mm
#     ../../logging.cpp
# )
set(osx_plugin_SRCS
    macdispatcher.cpp
    macdispatcher_helper.mm
    ../../logging.cpp
)

add_library(KF5IdleTimeOsxPlugin MODULE ${osx_plugin_SRCS})
target_link_libraries(KF5IdleTimeOsxPlugin
    KF5IdleTime
    Qt5::Widgets
    "-framework CoreFoundation -framework IOKit -framework AppKit"
)

install(
    TARGETS
        KF5IdleTimeOsxPlugin
    DESTINATION
        ${PLUGIN_INSTALL_DIR}/kf5/org.kde.kidletime.platforms/
)
