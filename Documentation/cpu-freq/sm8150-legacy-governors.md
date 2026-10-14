# Legacy CPU governor ports for SM8150

These experimental ports add selectable `despair`, `elementalx`,
`intelliactive` and `blu_active` governors to lineage-24.0. They adapt the
algorithms to the current CPUFreq infrastructure; they are not unchanged
copies of the Nexus 6P binaries and do not promise identical tuning.

## Sources and compatibility

Base: deani77/lineageos_kernel_oneplus_sm8150,
commit 0521dc291cf10bd754fb5ff385e74875c406d577.

Donors: deani77/kernel_huawei_angler_old, n7.1.1,
commit bfb980b49d2cb7304bf3fcf9fb5a399c03fe8e67, drivers/cpufreq/cpufreq_{despair,elementalx,intelliactive,blu_active}.c.

The current conservative and interactive implementations provide the 4.14
lifecycle, scheduler update hooks, policy locking and governor sysfs API.
Original GPLv2 notices and author attribution are retained. Separate trace
headers prevent duplicate tracepoint symbols when both interactive variants
are compiled together.

| Governor | Behavior retained | Compatibility changes |
| --- | --- | --- |
| despair | 95% up threshold, 35% down threshold, 5% frequency steps, intermediate-load two-step ramp | Current DBS callbacks; two-step count/time stored per policy; configurable intermediate threshold actually used; requests constrained to policy limits |
| elementalx | Load averaging, burst-to-max, six ramp curves, active frequency floor, powersave curve selection | Donor curves normalized to each policy's frequency range rather than indexing a fixed 12-entry hardware table; previous load and floor counters per policy; this changes the exact selected frequencies |
| intelliactive | Interactive target-load decisions, two-phase ramp, touch boost, longer optional hold at max frequency | Current interactive hooks/sysfs; scalar two_phase_freq per policy instead of global online-CPU array; phase counters per CPU; sampling_down_factor is a true multiplier (1–20); removed cross-policy sync_freq/up_threshold_any_cpu controls because frequencies from different SM8150 clusters are not comparable |
| blu_active | Interactive target-load decisions, hispeed delay, boost pulse, optional maximum-frequency hysteresis and fast restoration | Current interactive hooks/sysfs; obsolete sched_set_window/sched_get_busy and migration/load notifier interfaces removed; use_sched_load, use_migration_notif, align_windows, ignore_hispeed_on_notif and fast_ramp_down are not exposed; load comes from current idle accounting and scheduler-triggered evaluation |

The ports implement separate governor registrations and retain the current
kernel's frequency limits and CPUFreq driver. They do not add overclocking,
voltage controls, hotplug behavior, or scheduler replacements.

## Build and tuning

The LineageOS device build combines vendor/sm8150-perf_defconfig with
vendor/oplus.config. The latter enables all four ports as built-in governors.
The existing default-governor choice is unchanged; Android may select a
different governor after boot.

The new Kconfig options are CPU_FREQ_GOV_DESPAIR,
CPU_FREQ_GOV_ELEMENTALX, CPU_FREQ_GOV_INTELLIACTIVE and
CPU_FREQ_GOV_BLU_ACTIVE. Despair and ElementalX select the common DBS layer;
the interactive variants select the governor attribute and IRQ-work layers;
intelliactive also requires INPUT.

Additional tunables:

- despair: twostep_threshold (default 65), alongside sampling_rate,
  sampling_down_factor, up_threshold, down_threshold, ignore_nice_load and
  freq_step.
- elementalx: active_floor_freq (default 960000 kHz), powersave (0 or 1),
  up_threshold (90), down_differential (20), sampling_down_factor (4),
  sampling_rate and ignore_nice_load. The active floor is a temporary
  downscale hold, not a permanent minimum frequency.
- intelliactive: two_phase_freq (default midpoint of the policy's allowed
  frequency range; 0 bypasses the intermediate phase), input_boost (default
  enabled), sampling_down_factor (default 1), plus interactive tunables.
  Touch boost uses boostpulse_duration and bypasses the intermediate phase.
- blu_active: max_freq_hysteresis (microseconds, default 0), plus interactive
  tunables. Setting it to zero disables hysteresis.

The new interactive variants validate target-load divisors, bound tuning
arrays and durations, support timer_slack=-1, free replaced arrays at kobject
release, and stop the worker on registration failure. Kernel/ROM tuning scripts
must use the new interfaces rather than assuming every old sysfs setting exists.

## Validation

Validated on the development host:

- Kconfig resolution for the SM8150 base plus Oplus fragment: all four enabled.
- All four governor translation units compile to ARM64 ELF objects using the
  actual generated kernel headers and Kbuild compiler flags (LLVM 22.1.8,
  machine-object check with LTO disabled).
- DBS decision tests execute the actual Despair and ElementalX decision
  functions with mocked CPUFreq policies under address/undefined-behavior
  sanitizers. They cover load sweeps, three cluster ranges, restricted
  frequency limits, a single-frequency policy, two-step ramping, powersave
  curves and a concurrent-tuning denominator guard.
- Checkpatch and whitespace checks are performed before publishing.

Run the decision tests from the repository root:

```sh
python3 tools/testing/selftests/cpufreq/legacy_governors_test.py
```

The host checks do not validate concurrency, a complete linked kernel image,
boot behavior or performance on a phone. Full Linux/Android toolchain build
and OnePlus 7 device tests remain necessary before merging for daily use.
The macOS host's normal full Kbuild path lacks the Linux host environment and
ARM64 ELF linker; object checks reuse the generated ARM64 Kbuild flags instead.

On a test device, inspect each policy's scaling_available_governors and
scaling_governor. Exercise switching governors on all policies, governor
limits, CPU offline/online, suspend/resume and touch events; watch for lockdep
or kernel errors. Compare frame timing, battery use and temperature against
schedutil under the same workload before choosing defaults.
