{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  # Tools required for building
  nativeBuildInputs = with pkgs; [
    cmake
    pkg-config
    gcc
    git
    aseprite
    clang-tools
    ccache
  ];

  # Libraries the project depends on
  buildInputs = with pkgs; [
    curl
    curl.dev
    nlohmann_json
    raylib
    
    # Local text to speech, driven as a subprocess by src/PiperSpeech.cpp.
    #
    # `piper-tts` (piper1-gpl) and NOT `piper`, which is libratbag's gaming-mouse
    # configuration tool and has nothing to do with speech.
    #
    # Voice models are NOT part of this package. They are downloaded separately
    # into models/tts (gitignored) -- see SETTINGS.md.
    #
    # This pulls in more than the CLI needs: nixpkgs builds piper-tts with its
    # training, HTTP-server and alignment extras (withTrain/withHTTP/withAlignment
    # all default to true), which drags in PyTorch, pytorch-lightning, librosa and
    # Flask. Overriding them off is a one-line change --
    #
    #   (piper-tts.override { withTrain = false; withHTTP = false; withAlignment = false; })
    #
    # -- but that variant is not in the binary cache, so it builds piper-tts and
    # its patched espeak-ng from source on the first nix-shell. Worth doing when
    # the closure size starts to matter; not worth blocking a checkout on.
    piper-tts
    
    # Raylib dependencies
    #
    # alsa-lib and libpulseaudio are not just build inputs: raylib's miniaudio
    # dlopen()s its audio backend at runtime instead of linking it, and when that
    # dlopen fails it silently falls back to a NULL device -- IsAudioDeviceReady()
    # still returns true and PlaySound() still "plays", so the game looks healthy
    # and makes no sound. CMakeLists links both so that their store directories end
    # up in the binary's RUNPATH; src/SpeechQueue.cpp reports it loudly if neither
    # can be loaded.
    alsa-lib
    libpulseaudio
    libGL
    xorg.libX11
    xorg.libXcursor
    xorg.libXi
    xorg.libXinerama
    xorg.libXrandr
    xorg.libXext
  ];
  
  # Ensure pkg-config finds the .pc files
  shellHook = ''
    export NIX_CFLAGS_COMPILE="$NIX_CFLAGS_COMPILE -I${pkgs.nlohmann_json}/include"
  '';
}
