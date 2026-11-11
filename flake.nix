{
  description = "Hancom Office Hwp 2020 packaged natively for Nix (no Docker)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        config.allowUnfreePredicate =
          pkg:
          builtins.elem (nixpkgs.lib.getName pkg) [
            "hancom-hwp"
            "hancom-fonts"
          ];
      };
    in
    {
      packages.${system} = {
        hancom-hwp = pkgs.callPackage ./package.nix { }; # fcitx5 (default)
        hancom-hwp-ibus = pkgs.callPackage ./package.nix { inputMethod = "ibus"; };
        # bundled Korean fonts for other apps; override fontSets for more (see fonts.nix)
        hancom-fonts = pkgs.callPackage ./fonts.nix { inherit (self.packages.${system}) hancom-hwp; };
        default = self.packages.${system}.hancom-hwp;
      };

      apps.${system}.default = {
        type = "app";
        program = nixpkgs.lib.getExe self.packages.${system}.default;
      };

      overlays.default = final: prev: {
        hancom-hwp = final.callPackage ./package.nix { };
        hancom-fonts = final.callPackage ./fonts.nix { };
      };

      # Heavy: each check unpacks the full Hwp install and runs it under Xvfb.
      checks.${system} =
        let
          smoke = pkgs.callPackage ./tests/smoke.nix { };
        in
        {
          hancom-hwp-starts = smoke self.packages.${system}.hancom-hwp;
          hancom-hwp-ibus-starts = smoke self.packages.${system}.hancom-hwp-ibus;
        };

      formatter.${system} = pkgs.nixfmt;
    };
}
