# Valkey Design Documents

This folder collects designs for Valkey. These designs detail features and
changes that require more detail than just the text in a pull request or an
issue.

A markdown file describes each feature or larger topic.

## Major decision approval

Design documents can be used to get approval for larger features before
implementation. They can also be written after the fact, to document a feature
that is already implemented. A design document written after the fact is not a
major decision, since it describes existing behavior.

Before writing a design for a new feature, open an issue for early alignment.
The issue is the first step to find interested parties and collect initial
requirements, and serves as the overall tracking for the feature.

### Workflow for submitting a design document

1. After finishing initial discussion on the issue, determine the necessity of a
   design document. Small features or changes do not require designs.
   Wide-reaching changes or those requiring major alignment make good candidates
   for a design.
2. Once you've written the initial draft, open a pull request against `unstable`
   that adds a new markdown file in this `design-docs` directory.
3. Design documents are reviewed in the
   [weekly meeting](https://github.com/valkey-io/valkey/wiki/Weekly-meetings).
   You should reserve two slots in consecutive weeks. The first slot introduces
   the topic, and the second reviews the design in detail.
4. Adding a design document for a feature that is not yet implemented, or
   modifying one that is still pending, is a major decision. The pull request
   should be labeled `major-decision-pending` and is approved through the
   [technical major decision](../GOVERNANCE.md#technical-major-decisions) process.
5. Once its major decision is approved, a design document can be merged before
   its feature is implemented. Alternatively, the pull request can stay open
   with the implementation added to it, so the design and the code merge
   together.
6. A design document that is merged before its feature is fully implemented is
   pending. It begins with a section that says the design is a work in progress
   and lists its open questions. The pull request that completes the
   implementation removes that section.

### Implementing a design

The design document serves as living documentation. As developers build the
feature, the design document captures key design aspects. Commit changes to the
design document alongside the code changes as developers implement the mid-level
design details.

A pull request that implements a design as written does not need a major
decision. If a pull request diverges from the design, only the divergence is a
major decision. The pull request updates the design document to match, and is
labeled `major-decision-pending`.

## What's useful to include?

**Design documents do not follow a strict format.** There is no template.

When writing a design, consider that many people, including those unfamiliar
with the feature, read it. It should be self-contained and easy to understand.

The following sections are optional but provide a high-level overview of
potential inclusions:

- **High-level overview**: A brief summary of the feature and its purpose.
- **User-visible interfaces**: New or changed commands, configuration, and INFO
  fields, and how users are expected to use them, where appropriate.
- **Compatibility**: The overall strategy for compatibility with existing
  behavior and earlier versions.
- **Key design elements**: The following are generally useful to include:
  - State machines
  - Data structures
  - Algorithms
  - Interaction with other Valkey components (replication, persistence, cluster,
    modules, etc.)
- **Links to key issues/PRs**: Link to relevant issues/PRs for further reading.
- **Links to relevant code**: Link to relevant code files for further reading.

## What not to include?

**Design documents are not specifications.** They describe the design, not every
detail of the implementation. Overdocumentation often leads to stale design
documents. A design document is not the best place for the following:

- **Low-level Implementation Details**: Document difficult or complex
  implementation details in code comments, not design documents.
- **Edge Cases**: Describe the compatibility strategy, not how each edge case is
  handled. Link to test cases or code locations that cover edge cases instead.
- **Alternatives/Rejected Designs**: Document decision making in the issue or PR
  where the contributors made the decision.
- **Overly Verbose Explanations**: Aim to use Mermaid or ASCII diagrams to
  explain complex concepts rather than prose.
- **Boilerplate**: Keep every document minimal and to the point. Avoid
  unnecessary sections.
- **Future work**: File issues for future work items to track them, rather than
  including them in the design document.
