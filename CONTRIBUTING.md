Contributing to Valkey
======================

Welcome and thank you for wanting to contribute!

# Project governance

The Valkey project is led by a Technical Steering Committee, whose responsibilities are laid out in [GOVERNANCE.md](GOVERNANCE.md).

## Get started

* Have a question? Ask it on
  [GitHub Discussions](https://github.com/valkey-io/valkey/discussions)
  or [Valkey's Discord](https://discord.gg/zbcPa5umUB)
  or [Valkey's Matrix](https://matrix.to/#/#valkey:matrix.org)
* Found a bug? [Report it here](https://github.com/valkey-io/valkey/issues/new?template=bug_report.md&title=%5BBUG%5D)
* Valkey crashed? [Submit a crash report here](https://github.com/valkey-io/valkey/issues/new?template=crash_report.md&title=%5BCRASH%5D+%3Cshort+description%3E)
* Suggest a new feature? [Post your detailed feature request here](https://github.com/valkey-io/valkey/issues/new?template=feature_request.md&title=%5BNEW%5D)
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

# How to provide a patch or a new feature

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
    1. Initiate a pull request on GitHub ([HOWTO](https://docs.github.com/en/github/collaborating-with-issues-and-pull-requests/creating-a-pull-request))
    1. Done :)

3. Keep in mind that we are very overloaded, so issues and PRs sometimes wait
for a *very* long time. However this is not a lack of interest, as the project
gets more and more users, we find ourselves in a constant need to prioritize
certain issues/PRs over others. If you think your issue/PR is very important
try to popularize it, have other users commenting and sharing their point of
view, and so forth. This helps.

4. For minor fixes, open a pull request on GitHub.

To link a pull request to an existing issue, please write "Fixes #xyz" somewhere
in the pull request description, where xyz is the issue number.

Thanks!
