{
  inputs.nixpkgs.url = "https://flakehub.com/f/NixOS/nixpkgs/0";

  outputs =
    { self, ... }@inputs:
    let
      inherit (inputs.nixpkgs) lib;

      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];

      forEachSupportedSystem =
        f:
        lib.genAttrs supportedSystems (
          system:
          f {
            inherit system;
            pkgs = import inputs.nixpkgs {
              inherit system;
              config.allowUnfree = true;
            };
          }
        );
    in
    {
      devShells = forEachSupportedSystem (
        { pkgs, system }:
        {
          default = pkgs.mkShell {
            name = "sundae-env";

            packages = with pkgs; [
              clang
              cmake
              ninja
              pkg-config
              ffmpeg
              glfw
              sdl3
              SDL2
              SDL
            ];

            shellHook = ''
              export PS1="\[\e[1;32m\][sundae]\[\e[0m\] \[\e[1;34m\]\u@\h:\w\[\e[0m\]\$ "
            '';
          };
        }
      );
    };
}
