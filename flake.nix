{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable-small";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forEachSystem = f: nixpkgs.lib.genAttrs systems (system: f system);
    in
    {
      devShells = forEachSystem (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          vcpkgRoot = "${pkgs.vcpkg}/share/vcpkg";
        in
        {
          default = pkgs.mkShell {
            nativeBuildInputs = with pkgs; [
              autoconf
              autoconf-archive
              automake
              cmake
              git
              ibus
              libtool
              libX11
              libXext
              libXft
              libxkbcommon
              mesa
              vcpkg
              ninja
              nixfmt
              pkg-config
              stdenv.cc
              wayland
            ];
            VCPKG_ROOT = vcpkgRoot;
            shellHook = ''
              if [ -f vcpkg.json ] && ! grep -q '"builtin-baseline"' vcpkg.json; then
                vcpkg x-update-baseline --add-initial-baseline || exit 1
              fi
            '';
          };
        }
      );
      formatter = forEachSystem (system: nixpkgs.${system}.nixfmt);
    };
}
