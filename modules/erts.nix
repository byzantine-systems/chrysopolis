# packages.liberts, the cross-compiled ERTS emulator, archived as liberts.a.
#
# Only the emulator (erts/emulator/) is built, the OTP standard
# library is not needed until the BEAM bytecode is loaded from
# romfs. The source tarball is inherited from pkgs.erlang so
# its hash is already pinned in flake.lock.
#
# The preloaded .beam files (init, erlang, erts_internal, ...) ship
# precompiled in the OTP tarball and are converted to C byte-array
# tables by Perl scripts during the emulator build, no host erlc
# is required for this step.
{
  perSystem =
    { pkgs, chryso, ... }:
    {
      packages.liberts = chryso.targetPkgs.stdenv.mkDerivation {
        pname = "liberts";
        inherit (pkgs.beamPackages.erlang) version src;

        nativeBuildInputs = [ pkgs.perl ];
        # Build-host compiler, exported as $CC_FOR_BUILD. Needed to build
        # the YCF code generator (see buildPhase): it runs during the
        # cross build, so it must be a native x86 binary.
        depsBuildBuild = [ chryso.targetPkgs.buildPackages.stdenv.cc ];

        hardeningDisable = [ "all" ];
        dontStrip = true;

        configurePhase = ''
                        runHook preConfigure
                        export ERL_TOP=$(pwd)

                        # Supply facts OTP cannot determine by running test programs
                        # when cross-compiling for AArch64 LE musl.
                        cat > xcomp.conf <<'XCONF'
          erl_xcomp_bigendian=no
          erl_xcomp_double_middle_byte_order=no
          erl_xcomp_clock_gettime_cpu_time=yes
          erl_xcomp_after_morecore_hook=no
          erl_xcomp_reliable_fpe=no
          erl_xcomp_poll=yes
          erl_xcomp_epoll=yes
          erl_xcomp_timerfd=yes
          erl_xcomp_kqueue=no
          erl_xcomp_putenv_copy=no
          erl_xcomp_msgend_copy=no
          erl_xcomp_getaddrinfo=yes
          erl_xcomp_getnameinfo=yes
          XCONF

                        # The top-level ./configure reads erl_xcomp_* from the
                        # environment; --xcomp-conf is an otp_build-only flag, so we
                        # export the values ourselves instead of passing the file.
                        set -a
                        . ./xcomp.conf
                        set +a

                        ./configure \
                          --host=${chryso.targetPkgs.stdenv.hostPlatform.config} \
                          --build=${pkgs.stdenv.hostPlatform.config} \
                          --without-ssl \
                          --without-termcap \
                          --without-wx \
                          --without-javac \
                          --without-odbc \
                          --without-megaco \
                          --disable-dynamic-loading \
                          --disable-jit \
                          --disable-hipe

                        runHook postConfigure
        '';

        buildPhase = ''
          runHook preBuild

          # YCF (Yielding C Fun) rewrites some emulator .c files into
          # yielding form at build time, so it must run on the build host.
          # The OTP build's cross path resolves it via an escript
          # (utils/find_cross_ycf) that needs a bootstrap erlang we do not
          # have; instead we compile YCF natively here and override
          # YCF_EXECUTABLE_PATH so the emulator make uses our host binary.
          ycfsrc=erts/lib_src/yielding_c_fun
          mkdir -p "$TMPDIR/ycf"
          for c in "$ycfsrc"/ycf_*.c; do
            $CC_FOR_BUILD -I"$ycfsrc" -O2 -c "$c" \
              -o "$TMPDIR/ycf/$(basename "$c" .c).o"
          done
          $CC_FOR_BUILD -O2 -o "$TMPDIR/ycf/yielding_c_fun" \
            "$TMPDIR/ycf"/*.o

          # TARGET defaults to config.guess (the build triple); for a
          # cross build it must name the host dir configure generated
          # under erts/emulator/.
          make -j$NIX_BUILD_CORES -C erts/emulator \
            TARGET=${chryso.targetPkgs.stdenv.hostPlatform.config} \
            YCF_EXECUTABLE_PATH="$TMPDIR/ycf/yielding_c_fun"

          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          mkdir -p $out/lib $out/include/erts

          target=${chryso.targetPkgs.stdenv.hostPlatform.config}

          # OTP's own build already archives the emulator core into
          # libbeam.a from $(PRELOAD_OBJ) + $(OBJS): the preloaded BEAM
          # modules plus every emulator object including erl_init.o
          # (which defines erl_start). Crucially it omits erl_main.o, so
          # there is no main() to clash with Microkit's init() entry.
          # That is exactly the liberts.a we want.
          cp bin/$target/libbeam.a $out/lib/liberts.a
          $RANLIB $out/lib/liberts.a

          # Ship the emulator's bundled dependency archives (pcre, ryu,
          # zstd, zlib, micro-openssl, ethread, ...) so the liberts.a 
          # can resolve its symbols. Names are unique.
          # -n guards against any incidental basename collision.
          find . -path "*/$target/*" -name '*.a' ! -name 'libbeam.a' \
            -exec cp -n {} $out/lib/ \;

          # Generated + public emulator headers for erl_start()'s
          # declaration and any erl_*.h the translation layer includes.
          [ -d erts/include ] && cp -r erts/include/. $out/include/erts/ || true
          runHook postInstall
        '';
      };
    };
}
