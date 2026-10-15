Contributing to Valkey
======================

Welcome and thank you for wanting to contribute!

# Project governance

The Valkey project is led by a Technical Steering Committee, whose responsibilities are laid out in [GOVERNANCE.md](GOVERNANCE.md).

## Get started

* Have a question? Ask it on
  [GitHub Discussions](https://github.com/valkey-io/valkey/discussions),
  [Valkey's Slack](https://valkey.io/slack)
  or [Valkey's Matrix](https://matrix.to/#/#valkey:matrix.org)
* Found a bug? [Report it here](https://github.com/valkey-io/valkey/issues/new?template=bug_report.md&title=%5BBUG%5D)
* Valkey crashed? [Submit a crash report here](https://github.com/valkey-io/valkey/issues/new?template=crash_report.md&title=%5BCRASH%5D+%3Cshort+description%3E)
* Suggest a new feature? [Post your detailed feature request here](https://github.com/valkey-io/valkey/issues/new?template=feature_request.md&title=%5BNEW%5D)
* Report a test failure? [Report it here](https://github.com/valkey-io/valkey/issues/new?template=test-failure.md)
* Want to help with documentation? [Move on to valkey-doc](https://github.com/valkey-io/valkey-doc)
* Report a vulnerability? See [SECURITY.md](SECURITY.md)

## Developer Certificate of Origin

We respect the intellectual property rights of others and we want to make sure
all incoming contributions are correctly attributed and licensed. A Developer
Certificate of Origin (DCO) is a lightweight mechanism to do that. The DCO is
a declaration attached to every commit. In the commit message of the contribution,
the developer simply adds a `Signed-off-by` statement and thereby agrees to the DCO,
which you can find below or at [DeveloperCertificate.org](http://developercertificate.org/).

```text
Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the
    best of my knowledge, is covered under an appropriate open
    source license and I have the right under that license to
    submit that work with modifications, whether created in whole
    or in part by me, under the same open source license (unless
    I am permitted to submit under a different license), as
    Indicated in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including
    all personal information I submit with it, including my
    sign-off) is maintained indefinitely and may be redistributed
    consistent with this project or the open source license(s)
    involved.
```

We require that every contribution to Valkey to be signed with a DCO. We require the
usage of known identity (such as a real or preferred name). We do not accept anonymous
contributors nor those utilizing pseudonyms. A DCO signed commit will contain a line like:


```text
Signed-off-by: Jane Smith <jane.smith@email.com>
```

You may type this line on your own when writing your commit messages. However, if your
user.name and user.email are set in your git configs, you can use `git commit` with `-s`
or `--signoff` to add the `Signed-off-by` line to the end of the commit message. We also
require revert commits to include a DCO.

If you're contributing code to the Valkey project in any other form, including
sending a code fragment or patch via private email or public discussion groups,
you need to ensure that the contribution is in accordance with the DCO.

## Use of AI tools

You own the quality of the code that you submit to the project. The Valkey
project doesn't impose any limitations on the use of AI tooling as long as the
contribution meets the quality bar of the project.

## How to provide a patch or a new feature

1. If it is a major feature or a semantical change, please don't start coding
straight away: if your feature is not a conceptual fit you'll lose a lot of
time writing the code without any reason. Start by creating an issue at Github with the
description of, exactly, what you want to accomplish and why. Use cases are important for
features to be accepted. Here you can see if there is consensus about your idea.

2. If in step 1 you get an acknowledgment from the project leaders, use the following
procedure to submit a patch:
    1. Fork Valkey on GitHub ([HOWTO](https://docs.github.com/en/github/getting-started-with-github/fork-a-repo))
    1. Create a topic branch (`git checkout -b my_branch`)
    1. Make the needed changes and commit with a DCO. (`git commit -s`)
    1. Push to your branch (`git push origin my_branch`)
    1. Initiate a pull request on GitHub ([HOWTO](https://docs.github.com/en/github/collaborating-with-issues-and-pull-requests/creating-a-pull-request)).
       The sections below describe what reviewers look for and what happens next.

3. While developing code, make sure to refer to our [DEVELOPMENT_GUIDE.md](DEVELOPMENT_GUIDE.md),
which includes documentation about various best practices for writing Valkey code.

4. For minor fixes, open a pull request on GitHub.

To link a pull request to an existing issue, please write "Fixes #xyz" somewhere
in the pull request description, where xyz is the issue number.

## What makes a pull request easy to review

Reviewer time is the scarcest resource in the project. Pull requests that get
merged quickly tend to share these traits:

* **One change per pull request, and every pull request stands on its own.**
  Keep refactoring, formatting and functional changes in separate pull
  requests. For bug fixes, keep the diff as small as the fix allows: bug,
  security and test fixes are backported to release branches, and every extra
  line is a potential merge conflict there. Enhancements and new features are
  not backported, so size matters less, but each pull request must still leave
  `unstable` in a complete, working state with a benefit of its own. Do not
  split a feature into a series of pull requests where the early ones only make
  sense once the later ones land. To make a large change easier to review, use
  several well-described commits within one pull request instead; reviewers can
  read them one at a time, and the squash merge keeps `unstable` free of
  intermediate states.

* **A description that explains the why.** Start with the symptom or the gap,
  then the mechanism, then what the change does about it. For a bug fix, say
  how it can be reproduced and, if you know, which change introduced it.
  Before/after output or a short code excerpt is worth more than a long prose
  explanation.

* **Compatibility impact stated up front, even when there is none.** Call out
  anything that touches the RDB or AOF format, the replication stream, command
  replies (RESP2 and RESP3), `INFO` fields, configuration names or defaults,
  the module API, cluster bus messages, or ACL behavior. Reviewers check these
  surfaces on every pull request; saying "no compatibility impact" saves them
  the search.

* **Tests, and a sentence about what they cover.** See
  [DEVELOPMENT_GUIDE.md](DEVELOPMENT_GUIDE.md#test-coverage) for where tests
  live. The two test suites each have a README that specifies the expected
  style: [tests/README.md](tests/README.md) for the Tcl integration tests,
  including how to write tests that do not fail intermittently in CI, and
  [src/unit/README.md](src/unit/README.md) for the unit tests, which are
  written in minimal, C-readable C++ (no STL containers or algorithms, no
  `auto`, lambdas or templates). Reviewers will ask for changes to tests that
  do not follow these.

* **Numbers for performance or memory claims.** Name the workload (command mix,
  key count and value size, pipelining, threads, build flags, machine), run it
  more than once and report the spread, and show both sides of any tradeoff.
  A reviewer may apply the `run-benchmark` label to run the project's benchmark
  workflow on your branch.

* **A linked issue.** `Fixes #xyz` closes the issue on merge and gives the
  reviewer the discussion that led here. For a major feature, link the issue
  where the design was agreed.

## What to expect during review

* **Automated checks run first.** CI must be green before a maintainer will
  review in depth. This includes the DCO check, `clang-format-18`, the build
  matrix and the test suites. If this is your first contribution, most of the
  workflows will not start until a maintainer or committer approves the run;
  GitHub shows this as "workflows awaiting approval" on the pull request. Ask
  for the approval the same way you would ask for a reviewer (see below), and
  expect to ask again after each push until your first change is merged. A
  maintainer may also apply `run-extra-tests` or `run-valgrind-tests` to run
  the daily test matrix on your branch; tests that pass in the default CI
  configuration but fail under valgrind, sanitizers or TLS are the most common
  reason a merged change needs a follow-up fix.

* **Handle the AI review before asking for a human one.** An AI reviewer
  comments on most pull requests shortly after they are opened. Go through its
  comments before requesting review from a maintainer: fix what is right, and
  reply to what is wrong with a sentence on why, then resolve the thread. A
  human reviewer reads those threads too, and an unanswered one looks the same
  whether a bot or a person left it. AI reviewers are often wrong about intent
  and right about details, so a short "not applicable because X" is a
  legitimate and useful answer.

* **Review happens in inline threads.** Reviewers rarely use GitHub's "request
  changes" state; an open thread on your pull request is the signal that
  something needs to be addressed. Reply to every thread. If you applied the
  suggestion as written, say so and resolve the thread yourself; that is the
  normal flow and it keeps the reviewer's next pass short. If you did something
  different from what was asked, or you are declining the suggestion, reply
  with the reason and leave the thread open for the reviewer to close. A
  comment marked `nit:` or `(non-blocking)` is a suggestion you may decline.

* **Questions are feedback.** Reviewers often phrase a concern as "why not X?"
  or "shouldn't this be Y?". Treat these as requests to either change the code
  or explain the reasoning in the thread, since the explanation is what the
  next reader will need.

* **Expect to be asked to trim or defer.** A reviewer may ask for an unrelated
  improvement, a refactor, or a nice-to-have to come out of the pull request
  and land as its own change later. That is about keeping each change
  reviewable and backportable, not about rejecting the work; what remains must
  still be complete on its own.

* **Major decisions go to the TSC.** Changes to core data structures, new
  commands or APIs, backward-incompatible behavior, new user-visible fields and
  new external dependencies require Technical Steering Committee consensus (see
  [GOVERNANCE.md](GOVERNANCE.md)). A maintainer will apply the
  `major-decision-pending` label and the decision is tracked on the linked
  issue; review of the code itself continues in parallel. The TSC also holds a
  [weekly online meeting](https://github.com/valkey-io/valkey/wiki/Weekly-meetings)
  on Mondays that anyone can join. If your issue or pull request is waiting on
  a decision, add it to the next agenda on that page following the instructions
  there (the item must link to the issue or pull request), and join the call to
  answer questions. Conclusions are usually posted back as comments on the
  linked issue or pull request.

Labels you may see on your pull request:

| Label | Meaning |
| ----- | ------- |
| `needs-review` | Waiting for a maintainer to pick it up. |
| `pending-missing-dco` | A commit is missing the `Signed-off-by` line. |
| `major-decision-pending` / `-approved` / `-deferred` | TSC decision status, see above. |
| `needs-doc-pr` | The change needs a matching [valkey-doc](https://github.com/valkey-io/valkey-doc) pull request. Open it and the label is removed. |
| `release-notes` / `no-release-notes` | Whether the change gets a line in the release notes. |
| `run-extra-tests`, `run-valgrind-tests`, `run-benchmark` | A maintainer has asked CI to run the extended suites or the benchmark on your branch. |
| `to-be-merged` | Approved and waiting for a maintainer to merge. |
| `stalled` | No activity for a long time. Comment or push to reactivate. |

## Finding a reviewer

There is no automatic reviewer assignment. Maintainers watch the incoming
queue, and most pull requests get a first look within a day or two, but some
wait considerably longer when the queue is busy. To help your pull request
find the right reviewer:

* Request review from people who recently changed the files you touched
  (`git log --format=%an -- path/to/file | sort | uniq -c | sort -rn | head`).
  Reviewers listed in [MAINTAINERS.md](MAINTAINERS.md) can approve and merge;
  anyone may review.
* If nothing has happened after two weeks, leave a comment on the pull request
  summarizing its state and asking for a reviewer. For that, or for a CI
  approval, you can also ask in the
  [Valkey Slack](https://valkey.io/slack) or the
  [Valkey Matrix channel](https://matrix.to/#/#valkey:matrix.org), where
  maintainers and committers are active and a short message with the pull
  request link is enough. The
  [weekly online meeting](https://github.com/valkey-io/valkey/wiki/Weekly-meetings)
  is another option for a pull request that needs a discussion rather than a
  nudge. Pushing a rebase with no other change does not bring the pull request
  to anyone's attention.
* Reviewing other people's pull requests is one of the best ways to get
  involved and to learn the codebase. Reviews from non-maintainers are read
  and valued; a maintainer still gives the final approval.

## How a pull request gets merged

A pull request is merged by a maintainer once CI is green, every review thread
is resolved or answered, and it has an approval from a maintainer or committer.
Pull requests are squash-merged, so you do not need to squash or rewrite your
own commits during review; the pull request title and description become the
commit message, so keep them up to date as the change evolves. After the merge,
a maintainer decides whether the change is backported to release branches;
only bug, security and test fixes are eligible.

## Code formatting with clang-format

Valkey enforces code formatting using `clang-format-18`. A CI check runs on
every pull request and will fail if your code is not formatted correctly.

### Install clang-format-18

**Option A — pip (any platform):**

```bash
pip install clang-format==18.1.8
```

**Option B — apt (Debian/Ubuntu):**

```bash
sudo apt-get install software-properties-common -y
wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor | sudo tee /usr/share/keyrings/llvm-toolchain.gpg > /dev/null
echo "deb [signed-by=/usr/share/keyrings/llvm-toolchain.gpg] http://apt.llvm.org/$(lsb_release -cs)/ llvm-toolchain-$(lsb_release -cs)-18 main" | sudo tee /etc/apt/sources.list.d/llvm.list
sudo apt-get update -y
sudo apt-get install clang-format-18 -y
```

### Format your changes

Run clang-format on the files you modified:

```bash
clang-format-18 -i src/file_you_changed.c src/file_you_changed.h
```

To format all source files at once:

```bash
clang-format-18 -i src/*.c src/*.h
```

The formatting configuration lives in `src/.clang-format`. Use version 18
specifically — different versions may produce different output and cause the
CI check to fail.

## Running the daily workflow on demand for your branch

Use [`.github/workflows/daily.yml`](.github/workflows/daily.yml) with
`workflow_dispatch` to run daily tests manually on any branch in your fork.

1. Open your fork on GitHub and go to **Actions** -> **Daily**.
2. Click **Run workflow**.
3. In the **Branch** dropdown, select the branch that contains the workflow file you want to use.
4. In the input fields, set:
   * `use_repo` to your fork (for example, `your-user/valkey`)
   * `use_git_ref` to your branch name (or a specific commit SHA)
5. Optionally set `skipjobs`, `skiptests`, `test_args`, and `cluster_test_args`.
6. Click **Run workflow**.

Notes:
* To run the full matrix, set `skipjobs` and `skiptests` to `none`.
  Do not leave them empty, since the workflow input defaults may be applied.
* The scheduled part of this workflow is gated to `valkey-io/valkey`, but manual
  `workflow_dispatch` runs work for forks.

Thanks!
