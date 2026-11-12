{
  description = "Hancom Office (Hwp, Hcell, Hshow, Hword) packaged natively for Nix (no Docker)";

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
            "hancom-office"
            "hancom-fonts"
          ];
      };
    in
    {
      packages.${system} = {
        hancom-office = pkgs.callPackage ./package.nix { }; # fcitx5 (default)
        hancom-office-ibus = pkgs.callPackage ./package.nix { inputMethod = "ibus"; };
        # old names (the package now contains the whole office suite)
        hancom-hwp = self.packages.${system}.hancom-office;
        hancom-hwp-ibus = self.packages.${system}.hancom-office-ibus;
        # bundled Korean fonts for other apps; override fontSets for more (see fonts.nix)
        hancom-fonts = pkgs.callPackage ./fonts.nix { inherit (self.packages.${system}) hancom-office; };
        default = self.packages.${system}.hancom-office;
      };

      apps.${system} =
        let
          app = name: {
            type = "app";
            program = nixpkgs.lib.getExe' self.packages.${system}.default name;
          };
        in
        {
          default = app "hwp";
          hwp = app "hwp";
          hcell = app "hcl";
          hshow = app "hsl";
          hword = app "hword";
        };

      overlays.default = final: prev: {
        hancom-office = final.callPackage ./package.nix { };
        hancom-hwp = final.hancom-office;
        hancom-fonts = final.callPackage ./fonts.nix { };
      };

      formatter.${system} = pkgs.nixfmt;
    };
}
