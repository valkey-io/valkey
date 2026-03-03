# Valkey Design Documents

This folder is a collection of designs for Valkey. These designs are for
features and changes that require more detail than just the text in a pull
request or an issue.

Each feature or larger topic is described in a markdown file.

## Workflow

**IMPORTANT: Before writing a design, start an issue for some early alignment.**
This is the first step to find interested parties and collect initial
requirements. This issue will serve as the overall tracking for the feature.

1. Once initial discussion on the issue finishes, determine if a design document
   is needed or not. Designs are not required for small features or changes. If
   your change is wide reaching or requires major alignment, it is a good
   candidate for a design.
2. To create a design document, create a new markdown file in this `design-docs`
   directory. Use the [template](TEMPLATE.md) to structure your design document.
3. The maintainers will review and approve the design document once feedback is
   addressed. Note that submitting a design document is not necessarily a
   binding commit to a certain design.
4. The design document is intended to be living documentation. As the feature is
   developed, the design document is expected to capture key design aspects.
   Changes to the design document can be committed alongside the code changes as
   the mid-level design details are implemented.

Each file has one of the following statuses:

- **Proposed**, design has been proposed, and if merged, accepted.
- **Partially Implemented**, design has been accepted, and some changes have
  been implemented.
- **Implemented**, design has been accepted, and all changes have been
  implemented.

The core team can change the status and make changes. For larger changes, the PR
making the change is mentioned too and can be referred to by their respective
pull-request numbers.

## What's useful to include?

Design documents are not a strict format, but should include the following
sections unless they are unnecessary for the proposal you are submitting.

- Status.
- Abstract. A few sentences describing the feature.
- Motivation. What the feature solves and why the existing functionality is not
  enough.
- Design considerations. A description of the design constraints and
  requirements for the proposal. Comparisons with similar features in other
  projects.
- Specification. A more detailed description of the feature, including why
  certain details in the design have been chosen.
- Links to related material such as issues, pull requests, papers, or other
  references.

Here's a [design template](TEMPLATE.md) to get started.
