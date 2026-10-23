// PROBE ONLY (branch probe/metal-present-log): never vendor to the app's
// main line. One os_log line per Metal frame event, on the
// CACurrentMediaTime() clock.
#pragma once
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

/// The id of the last drawInMTKView (the real Metal frame).
extern uint64_t MLNProbeDrawID;

#define MLNPROBE(fmt, ...) \
  NSLog(@"MLNPROBE t=%.6f id=%llu " fmt, CACurrentMediaTime(), MLNProbeDrawID, ##__VA_ARGS__)
