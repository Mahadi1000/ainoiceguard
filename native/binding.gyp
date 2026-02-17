{
  "targets": [
    {
      "target_name": "noiseguard",
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "sources": [
        "src/addon.cc",
        "src/audio.cpp",
        "src/rnnoise_wrapper.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "src",
        "../deps/install/include/portaudio",
        "../deps/install/include/rnnoise"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        "NODE_ADDON_API_ENABLE_MAYBE"
      ],
      "conditions": [
        [
          "OS=='win'",
          {
            "libraries": [
              "-l../deps/install/lib/portaudio_static_x64.lib",
              "-l../deps/install/lib/rnnoise.lib",
              "-lole32.lib",
              "-lwinmm.lib",
              "-luuid.lib",
              "-lksuser.lib",
              "-ladvapi32.lib"
            ],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "AdditionalOptions": ["/std:c++17"]
              }
            },
            "defines": [
              "_WIN32",
              "PA_USE_WASAPI=1"
            ]
          }
        ],
        [
          "OS=='linux'",
          {
            "libraries": [
              "-L../deps/install/lib",
              "-lportaudio",
              "-lrnnoise",
              "-lasound",
              "-lpthread",
              "-lm"
            ],
            "cflags_cc": ["-std=c++17", "-fexceptions"]
          }
        ],
        [
          "OS=='mac'",
          {
            "libraries": [
              "-L../deps/install/lib",
              "-lportaudio",
              "-lrnnoise",
              "-framework CoreAudio",
              "-framework AudioToolbox",
              "-framework AudioUnit",
              "-framework CoreServices"
            ],
            "xcode_settings": {
              "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
              "CLANG_CXX_LANGUAGE_STANDARD": "c++17"
            }
          }
        ]
      ]
    }
  ]
}
