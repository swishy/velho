# Allocator CI and releases

The [Malloc workflow](../.github/workflows/malloc.yml) runs on every branch push,
pull request and manual dispatch. It builds and tests the standalone allocator
on Ubuntu 24.04 x86-64 at O0, O2 and O3. There are no path filters: changes to
shared headers also exercise the suite. A multi-commit push tests its resulting
revision and publishes that revision, rather than building each intermediate
commit separately.

The checked-in [test runner](../usr/lib/c/test-malloc.sh) runs hash sharing and
recycling, all eight allocator stress stages, alignment/capacity/realloc checks,
eight-thread churn, cross-thread frees and parked-owner frees. It also preloads
the library into the system Perl for a 30,000-entry churn smoke test. Each stress
process has a timeout; any failure fails the job. This smoke test is distinct
from the 237-file source-built Perl core subset recorded in the adoption report.
CI does not enforce wall-clock performance thresholds on shared runners.

After **all three matrix jobs succeed**, a push to `main` publishes a GitHub
release tagged `malloc-<full commit SHA>`. It uses the exact tested O2 binary
from that workflow run. Branch/PR/manual runs retain downloadable artifacts;
only pushes to `main` publish releases. Rerunning a successful main workflow
refreshes that commit's release assets without creating another release.

Release assets are `velhomalloc-linux-x86_64.tar.gz` and `SHA256SUMS`. The archive
includes `libvelhomalloc.so`, the license, design notes, usage instructions and
compiler/glibc/commit metadata. This is a Linux x86-64/glibc build from Ubuntu
24.04; other architectures and older glibc environments need their own builds.

GitHub provides `GITHUB_TOKEN` automatically. Tests use read access; only the
main release job requests `contents: write`. Actions must be enabled and the
repository/organization policy must permit that release permission and the
official checkout/upload/download actions. No personal token is required.
See [GitHub's release CLI documentation](https://cli.github.com/manual/gh_release_create)
for the publication command. The workflow must reach `main` before main pushes
can publish it; pushing this configuration to a feature branch starts tests.

To require successful CI before merging, select `Allocator O0`, `Allocator O2`
and `Allocator O3` as required status checks in the main branch's ruleset after
their first GitHub run. A workflow file alone does not configure branch rules.

Local reproduction requires a Linux x86-64 C toolchain, pthreads, GNU `timeout`
and Perl with `Time::HiRes` (available in the CI image):

```sh
usr/lib/c/build-malloc.sh /tmp/velho-malloc-build -O2
usr/lib/c/test-malloc.sh /tmp/velho-malloc-build
```

CI preserves build and test logs for 14 days, including failed runs. Successful
O2 packages also remain as workflow artifacts for 14 days; GitHub release assets
persist independently of that artifact retention period.
