Contributing to Valkey
======================

Welcome and thank you for wanting to contribute!

# Project governance

The Valkey project is led by a Technical Steering Committee, whose responsibilities are laid out in [GOVERNANCE.md](GOVERNANCE.md).

## Get started

* Have a question? Ask it on
  [GitHub Discussions](https://github.com/valkey-io/valkey/discussions)
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

While developing code, make sure to refer to our [DEVELOPMENT_GUIDE.md](DEVELOPMENT_GUIDE.md),
which includes documentation about various best practices for writing Valkey code.

### Submitting a patch

If your change is conceptually simple, like a bug fix or a small feature, you
can open a pull request directly. For a large feature, start with
[Large features](#large-features) before writing any code.

To submit a patch:

1. Fork Valkey on GitHub ([HOWTO](https://docs.github.com/en/github/getting-started-with-github/fork-a-repo)).
1. Create a branch (`git checkout -b my_branch`).
1. Make your changes and commit them with a DCO (`git commit -s`).
1. Push the branch to your fork (`git push origin my_branch`).
1. Open a pull request against `unstable` ([HOWTO](https://docs.github.com/en/github/collaborating-with-issues-and-pull-requests/creating-a-pull-request)).
   If it fixes an existing issue, write "Fixes #xyz" in the pull request
   description, where xyz is the issue number.

Automated review runs before a maintainer reviews the pull request. Please
address or reply to its comments.

### Large features

If the feature is large, changing many systems in Valkey or introducing new
classes of commands or data types, please don't start coding straight away.
Start by creating an issue on GitHub with a description of what you want to
accomplish and why. Focus on the end user experience, and leave out low-level
technical details unless they affect it. Use cases are important for features
to be accepted. Some large features also need a design document, which follows
the process in [design-docs/README.md](design-docs/README.md). The project
maintainers will let you know if that step is necessary.

Once you get an acknowledgment from the project maintainers, submit a patch using the
procedure in [Submitting a patch](#submitting-a-patch).

Keep in mind that we are very overloaded, so issues and PRs sometimes wait
for a *very* long time. However this is not a lack of interest, as the project
gets more and more users, we find ourselves in a constant need to prioritize
certain issues/PRs over others. If you think your issue/PR is very important
try to popularize it, have other users commenting and sharing their point of
view, and so forth. This helps.

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
