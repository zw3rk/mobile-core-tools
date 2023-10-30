{
  description = "mobile core tools: mac2ios";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-23.05";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      flake = {
        # ...
      };
      systems = [
        # systems for which you want to build the `perSystem` attributes
        "aarch64-linux"
        "x86_64-linux"
        "aarch64-darwin"
        "x86_64-darwin"
        # ...
      ];
      perSystem = { pkgs, self', ... }: {
        packages.mac2ios = pkgs.stdenv.mkDerivation {
          name = "mac2ios";
          src = ./.;
          # buildInputs = [ ];
          # buildPhase = ''
          #   make
          # '';
          installPhase = ''
            mkdir -p $out/bin
            cp mac2ios $out/bin/
          '';
        };

        packages.default = self'.packages.mac2ios;
      };
  };
}
