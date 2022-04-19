project(
 'tools',
 'c',
 version: '0.0.1',
 license: 'GPL-2.0-or-later',
)
deps = dependency('threads')

executable(
   'pcie_clock',
   'src/ptp_getclock.c', 'src/ftrace.c',
   build_by_default: true,
)

executable(
   'noisegen',
   'src/noise_gen.c',
   build_by_default: true,
	dependencies: deps)
