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
   directory.
3. The maintainers will review and approve the design document once feedback is
   addressed. Note that submitting a design document is not necessarily a
   binding commit to a certain design.
4. The design document is intended to be living documentation. As the feature is
   developed, the design document is expected to capture key design aspects.
   Changes to the design document can be committed alongside the code changes as
   the mid-level design details are implemented.

## What's useful to include?

**Design documents are not a strict format.** There is no template.

When writing a design, consider that it will be read by many people, including
people who are not familiar with the feature. It should be self-contained and
easy to understand.

The following sections are not required, but give a high level overview of what
could be included in a design document:

- **High-level overview**: A brief summary of the feature and its purpose.
- **Key design elements**: The following are generally useful to include:
  - State machines
  - Data structures
  - Algorithms
  - Interaction with other Valkey components (replication, persistence, cluster,
    modules, etc.)
- **Links to key issues/PRs**: Link to relevant issues/PRs for further reading.
- **Links to relevant code**: Link to relevant code files for further reading.

## What not to include?

Overdocumentation often leads to stale design documents. A design document is
not the best place for the following:

- **API Details**: API details belong in the
  [public Valkey documentation](https://valkey.io/commands/)
- **Low-level Implementation Details**: Difficult or complex implementation
  details should be documented in code comments, not design documents.
- **Edge Cases**: Prefer to link to test cases or code locations that cover edge
  cases, rather than documenting them in the design document.
- **Alternatives/Rejected Designs**: Decision making should be documented in the
  issue or PR where the decision was made.
- **Overly Verbose Explanations**: Aim to use Mermaid or ASCII diagrams to
  explain complex concepts rather than prose.
- **Boilerplate**: Every document should be minimal and to the point. Avoid
  unnecessary sections.
- **Future work**: File issues for future work items so they can be tracked,
  rather than including them in the design document.
