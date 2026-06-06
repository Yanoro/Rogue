{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  # Tools required for building
  nativeBuildInputs = with pkgs; [
    cmake
    pkg-config
    gcc
    git
    aseprite
  ];

  # Libraries the project depends on
  buildInputs = with pkgs; [
    curl
    curl.dev
    nlohmann_json
    raylib
    
    # Raylib dependencies
    alsa-lib
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
