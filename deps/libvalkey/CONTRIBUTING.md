# Contributing

:tada: Thanks for taking the time to contribute! :tada:

Contributions from the community are welcome. Feel free to open a PR with a bugfix or new feature.
Note that if you would like to merge a large feature it's probably a good idea to open an issue first rather than spending a lot of time on a PR that may not be accepted.

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

We require that every contribution to libvalkey to be signed with a DCO. We require the
usage of known identity (such as a real or preferred name). We do not accept anonymous
contributors nor those utilizing pseudonyms. A DCO signed commit will contain a line like:

```text
Signed-off-by: Jane Smith <jane.smith@email.com>
```

You may type this line on your own when writing your commit messages. However, if your
user.name and user.email are set in your git configs, you can use `git commit` with `-s`
or `--signoff` to add the `Signed-off-by` line to the end of the commit message. We also
require revert commits to include a DCO.

If you're contributing code to the libvalkey project in any other form, including
sending a code fragment or patch via private email or public discussion groups,
you need to ensure that the contribution is in accordance with the DCO.

## Coding conventions

### Code style

Adhere to the existing coding style and make sure to mimic best possible.

### Code formatting

When making a change, please use `git clang-format` or [format-files.sh](./scripts/format-files.sh) to format your changes properly.
This repository is currently using `clang-format` 18.1.3 to format the code, which can be installed using `pip install clang-format==18.1.3` or other preferred method.

## Running cluster tests

Prerequisites:

* Build `libvalkey` using CMake.
* Perl with [JSON module](https://metacpan.org/pod/JSON). Can be installed using `sudo cpan JSON`.
* [Docker](https://docs.docker.com/engine/install/)

Some tests needs a Valkey Cluster which can be setup using build targets.
The clusters will be setup using Docker and it may take a while for them to be ready and accepting requests.
Run `make start` to start the clusters and then wait a few seconds before running `make test`.
To stop the running cluster containers run `make stop`.
