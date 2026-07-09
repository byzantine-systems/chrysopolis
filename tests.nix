# NixOS-test-driver integration tests for the seL4/Microkit images.
#
# The guest is NOT a NixOS machine: the seL4 system image boots via QEMU's
# `-device loader` and speaks only over the PL011 serial console. We still run
# it under the NixOS test framework (pkgs.testers.runNixOSTest) for its Python
# driver, machine lifecycle, captured/structured console logging, timeouts and
# retries as real code instead of expect scripts. The seL4 QEMU is attached
# with driver.create_machine(<raw qemu command>), the mechanism the framework
# provides for non-NixOS guests.
#
# Driver contract for create_machine (nixos/lib/test-driver, machine/__init__.py):
#   - The start command must NOT set -serial/-monitor/-display/-nographic:
#     the driver appends `-serial stdio`, `-monitor unix:...`, a virtio-serial
#     "shell" chardev (PCI devices the seL4 guest simply ignores) and
#     -nographic itself. It must stay ONE line: the driver string-appends its
#     flags, so a trailing backslash-newline would orphan them.
#   - The command runs via `sh -c` with cwd = the machine's private state
#     directory, so the writable FAT disk copy lands there.
#   - Console capture is line-buffered: an unterminated prompt like `1> ` is
#     not observable until a later newline flushes it. Tests therefore key on
#     complete lines (the "Eshell V..." banner), never on the prompt.
#   - wait_for_console_text() consumes a shared queue (order-sensitive across
#     calls, and at most one line per 1s retry tick when given a timeout), so
#     the scripts poll machine.get_console_log() instead, order-insensitive
#     and immune to interleaved output from different PDs.
#
# Networking matches the old expect tests: QEMU user-mode (slirp), guest
# 10.0.2.15 via lwIP DHCP, host is 10.0.2.2. The test driver process *is* the
# slirp host, so the TCP peers are plain Python sockets in the test script
# (hostfwd tcp::5555 for host->guest, a listener on 127.0.0.1:5566 for
# guest->host). Fixed ports are safe: each check runs in its own sandbox netns.
{
  pkgs,
  sel4SystemImage,
  sel4TestImage,
  fatDisk,
}:
let
  qemu = "${pkgs.qemu}/bin/qemu-system-aarch64";

  # Disk on virtio-mmio bus.1, NIC on bus.0: the buses are pinned in
  # tools/sdf/system.zig, the drivers fault at virtio_transport_probe if a
  # device is missing or lands on another slot. The store disk is read-only,
  # and ERTS needs a writable FAT volume, hence the copy.
  startCommand =
    { image, netdev }:
    "cp ${fatDisk} disk.img && chmod u+w disk.img && exec ${qemu}"
    + " -machine virt,virtualization=on -cpu cortex-a53 -m size=2G"
    + " -device loader,file=${image}/sel4-beam.img,addr=0x70000000,cpu-num=0"
    + " -global virtio-mmio.force-legacy=false"
    + " -drive file=disk.img,if=none,format=raw,id=hd"
    + " -device virtio-blk-device,drive=hd,bus=virtio-mmio-bus.1"
    + " -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0"
    + " -netdev ${netdev}"
    + " -d guest_errors";

  # Shared test-script preamble: boot the seL4 machine and provide
  # wait_console (see the header comment for why not wait_for_console_text).
  preamble =
    { image, netdev }:
    ''
      import re
      import time

      def wait_console(machine, regex, timeout):
          """Wait until `regex` matches the accumulated console log."""
          with machine.nested(f"waiting for {regex!r} on console"):
              deadline = time.time() + timeout
              while time.time() < deadline:
                  if re.search(regex, machine.get_console_log()):
                      return
                  time.sleep(1)
          raise Exception(f"timed out waiting for {regex!r} on console")

      def assert_no_pd_fault(machine):
          """MON|ERROR on serial means a protection domain faulted."""
          assert "MON|ERROR" not in machine.get_console_log(), \
              "a PD faulted (MON|ERROR in serial log)"

      chryso = create_machine(
          "${startCommand { inherit image netdev; }}",
          name="chrysopolis",
      )
      chryso.start()
    '';

  mkSel4Test =
    {
      name,
      image,
      netdev ? "user,id=net0",
      testScript,
    }:
    pkgs.testers.runNixOSTest {
      inherit name;
      nodes = { };
      # The script drives a create_machine() guest, which the type checker
      # cannot see (there are no declared nodes).
      skipTypeCheck = true;
      testScript = preamble { inherit image netdev; } + testScript;
    };
in
{
  # Step gates for the ERTS image: beam_server boots on the LionsOS reference
  # stack, ERTS launches, the blk stack validates the FAT disk, and the Eshell
  # comes up having loaded kernel+stdlib through the FAT fs_server. Milestones
  # are asserted from the accumulated log (PD output interleaves, so only the
  # final milestone is waited on).
  boot-smoke = mkSel4Test {
    name = "boot-smoke";
    image = sel4TestImage;
    testScript = ''
      wait_console(chryso, r"Eshell", 300)
      log = chryso.get_console_log()
      for milestone in [
          "beam_server up on the LionsOS reference stack.",
          "monotonic clock via sDDF timer:",
          "Handing off to ERTS core loop...",
          "MBR partitioning detected",
      ]:
          assert milestone in log, f"missing boot milestone: {milestone}"
      assert_no_pd_fault(chryso)
      chryso.crash()
    '';
  };

  # Bring-up image (no ERTS): beam_run()'s SOCKET_SMOKE cothread brings up the
  # linked lwIP stack, gets a DHCP lease over the sDDF net path and exercises
  # socket()/bind()/listen()/connect() from C. Pins the lwip-socket-client
  # criterion (sockets work before ERTS is involved).
  socket-smoke = mkSel4Test {
    name = "socket-smoke";
    image = sel4SystemImage;
    testScript = ''
      wait_console(chryso, r"SOCKET_SMOKE\|PASS", 120)
      assert "SOCKET_SMOKE|DHCP:" in chryso.get_console_log(), \
          "no DHCP lease before the socket self-test passed"
      assert_no_pd_fault(chryso)
      chryso.crash()
    '';
  };

  # Interactive Eshell: evaluate `1 + 1.` over the serial console and expect 2
  # (colourised `2\e[0m` on a tty, or a bare 2 on its own line).
  shell-smoke = mkSel4Test {
    name = "shell-smoke";
    image = sel4TestImage;
    testScript = ''
      wait_console(chryso, r"Eshell", 300)
      time.sleep(2)  # banner precedes the prompt; let the line editor come up
      chryso.send_console("1 + 1.\r")
      wait_console(chryso, r"(?m)2\x1b\[0m|^2$", 60)
      assert_no_pd_fault(chryso)
      chryso.crash()
    '';
  };

  # Real entropy (issue 0.3.0-rng): the jitter-seeded HMAC-DRBG makes RNG and
  # time-seeded values vary across boots. Boot the (default topology) ERTS image
  # TWICE and assert the RNG fingerprint, rand:bytes/1 and erlang:make_ref/0 all
  # differ between the two boots. The image and QEMU command line are unchanged
  # from the other ERTS tests (Phase 1 adds no PD and no QEMU device), so this is
  # purely a second boot, not a new topology.
  rng-smoke = mkSel4Test {
    name = "rng-smoke";
    image = sel4TestImage;
    testScript = ''
      def boot_and_capture(machine):
          # The RNG| line is printed by rng_init() before ERTS hands off.
          wait_console(machine, r"RNG\|source=", 300)
          wait_console(machine, r"Eshell", 300)
          time.sleep(2)  # banner precedes the prompt; let the line editor come up
          # One console line (writes are ~1s each under TCG) that also proves the
          # openat shim: open /dev/urandom raw and read a BOUNDED 8 bytes
          # (file:read_file would loop forever — /dev/urandom never EOFs). Tagged,
          # space-separated prints so each value is unambiguous in the log.
          machine.send_console(
              '{ok,Fd}=file:open("/dev/urandom",[read,binary,raw]), {ok,U}=file:read(Fd,8), file:close(Fd), io:format("RNG_BYTES|~w RNG_REF|~p RNG_URANDOM|~w~n",[rand:bytes(8),erlang:make_ref(),U]).\r'
          )
          wait_console(machine, r"RNG_URANDOM\|", 60)
          assert_no_pd_fault(machine)
          return machine.get_console_log()

      def extract(log, pat):
          hits = re.findall(pat, log)
          assert hits, f"pattern {pat!r} never appeared in the console log"
          return hits[-1]

      # Boot 1 is the machine the preamble already started.
      log1 = boot_and_capture(chryso)
      chryso.crash()

      # Boot 2: a fresh machine, same command line (re-copies the FAT disk).
      chryso2 = create_machine(
          "${
            startCommand {
              image = sel4TestImage;
              netdev = "user,id=net0";
            }
          }",
          name="chrysopolis-2",
      )
      chryso2.start()
      log2 = boot_and_capture(chryso2)
      chryso2.crash()

      fp1 = extract(log1, r"RNG\|source=\S+\|fp=([0-9a-f]+)")
      fp2 = extract(log2, r"RNG\|source=\S+\|fp=([0-9a-f]+)")
      assert fp1 != fp2, f"RNG fingerprint identical across boots ({fp1})"

      b1 = extract(log1, r"RNG_BYTES\|(\S+)")
      b2 = extract(log2, r"RNG_BYTES\|(\S+)")
      assert b1 != b2, f"rand:bytes(8) identical across boots ({b1})"

      r1 = extract(log1, r"RNG_REF\|(\S+)")
      r2 = extract(log2, r"RNG_REF\|(\S+)")
      assert r1 != r2, f"erlang:make_ref() identical across boots ({r1})"

      # /dev/urandom read succeeded (openat shim + read callback), non-static.
      u1 = extract(log1, r"RNG_URANDOM\|(\S+)")
      u2 = extract(log2, r"RNG_URANDOM\|(\S+)")
      assert u1 != u2, f"/dev/urandom returned identical bytes across boots ({u1})"
    '';
  };

  # gen_tcp end-to-end: TCP both ways over the real stack:
  #
  # ERTS inet_drv -> libc sock.c -> src/runtime/tcp.c -> lwIP ->
  # sDDF net -> virtio-net/slirp.
  #
  #   1. host->guest: a looping gen_tcp echo server on :5555 (hostfwd), the
  #      test connects from Python, sends a payload, and must get it back.
  #      The echo hot path is io:format-free: each console write costs ~1s
  #      under TCG and two of them blow the 3s socket timeout. ECHOED
  #      printing *after* close also regression-tests the late-ACK crash.
  #   2. guest->host: gen_tcp:connect to a Python listener via the slirp
  #      gateway 10.0.2.2, which must receive the payload.
  tcp-smoke = mkSel4Test {
    name = "tcp-smoke";
    image = sel4TestImage;
    netdev = "user,id=net0,hostfwd=tcp::5555-:5555";
    testScript = ''
      import socket

      # slirp only forwards to the guest once lwIP holds its 10.0.2.15 lease
      # (packets to other dest IPs are dropped), so require DHCP before the
      # shell is driven. Log polling makes DHCP-vs-banner order irrelevant.
      wait_console(chryso, r"SOCKET_SMOKE\|DHCP:", 300)
      wait_console(chryso, r"Eshell", 300)
      time.sleep(2)

      # Looping echo server: accept, recv, echo, close, repeat, early
      # half-open probes can't consume a one-shot acceptor. {packet,raw}:
      # the payload is raw bytes, not line-framed.
      chryso.send_console(
          'spawn(fun() -> {ok,L}=gen_tcp:listen(5555,[binary,{packet,raw},{active,false},{reuseaddr,true}]), io:format("LISTENER_UP~n"), (fun F() -> case gen_tcp:accept(L,30000) of {ok,S} -> case gen_tcp:recv(S,0) of {ok,B} -> gen_tcp:send(S,B), gen_tcp:close(S), io:format("ECHOED ~p~n",[B]); E -> io:format("RECV_ERR ~p~n",[E]) end; E2 -> io:format("ACCEPT_ERR ~p~n",[E2]) end, F() end)() end).\r'
      )
      wait_console(chryso, r"LISTENER_UP", 60)
      time.sleep(3)  # let the acceptor settle

      # 1. host -> guest echo. Retries: the guest may still be settling and
      # refuse the first connections.
      payload = b"CHRYSO_ECHO"
      echoed = None
      for attempt in range(30):
          try:
              with socket.create_connection(("127.0.0.1", 5555), timeout=3) as s:
                  s.settimeout(3)
                  s.sendall(payload)
                  buf = b""
                  while len(buf) < len(payload):
                      chunk = s.recv(1024)
                      if not chunk:
                          break
                      buf += chunk
                  if buf == payload:
                      echoed = buf
                      break
                  chryso.log(f"echo attempt {attempt}: got {buf!r}")
          except OSError as err:
              chryso.log(f"echo attempt {attempt}: {err}")
          time.sleep(2)
      assert echoed == payload, "host->guest gen_tcp echo never round-tripped"
      # The guest-side ECHOED print lands after gen_tcp:close, its absence
      # (or a MON|ERROR) means the close-with-in-flight-ACK path regressed.
      wait_console(chryso, r"ECHOED", 60)

      # 2. guest -> host connect through the slirp gateway.
      srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
      srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
      srv.bind(("127.0.0.1", 5566))
      srv.listen(1)
      srv.settimeout(120)
      chryso.send_console(
          'case gen_tcp:connect({10,0,2,2},5566,[binary,{active,false}],5000) of {ok,C} -> gen_tcp:send(C,<<"CHRYSO_PING">>), gen_tcp:close(C), io:format("TCP_CONNECT_OK~n"); E3 -> io:format("TCP_CONNECT_ERR ~p~n",[E3]) end.\r'
      )
      conn, _addr = srv.accept()
      conn.settimeout(30)
      got = b""
      while True:
          chunk = conn.recv(1024)
          if not chunk:
              break
          got += chunk
      conn.close()
      srv.close()
      assert got == b"CHRYSO_PING", f"host listener received {got!r}"
      wait_console(chryso, r"TCP_CONNECT_OK", 60)

      assert_no_pd_fault(chryso)
      chryso.crash()
    '';
  };
}
