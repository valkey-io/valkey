# Valkey Design Documents

This folder collects designs for Valkey. These designs detail features and
changes that require more detail than just the text in a pull request or an
issue.

A markdown file describes each feature or larger topic.

## Workflow

**IMPORTANT: Before writing a design, start an issue for some early alignment.**
This is the first step to find interested parties and collect initial
requirements. This issue serves as the overall tracking for the feature.

1. After finishing initial discussion on the issue, determine the necessity of a
   design document. Small features or changes do not require designs.
   Wide-reaching changes or those requiring major alignment make good candidates
   for a design.
2. To create a design document, create a new markdown file in this `design-docs`
   directory.
3. The maintainers review and approve the design document once authors address
   feedback. Submitting a design document does not necessarily bind to a certain
   design.
4. The design document serves as living documentation. As developers build the
   feature, the design document captures key design aspects. Commit changes to
   the design document alongside the code changes as developers implement the
   mid-level design details.

## What's useful to include?

**Design documents do not follow a strict format.** There is no template.

When writing a design, consider that many people, including those unfamiliar
with the feature, read it. It should be self-contained and easy to understand.

The following sections are optional but provide a high-level overview of
potential inclusions:

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
- **Low-level Implementation Details**: Document difficult or complex
  implementation details in code comments, not design documents.
- **Edge Cases**: Prefer to link to test cases or code locations that cover edge
  cases, rather than documenting them in the design document.
- **Alternatives/Rejected Designs**: Document decision making in the issue or PR
  where the contributors made the decision.
- **Overly Verbose Explanations**: Aim to use Mermaid or ASCII diagrams to
  explain complex concepts rather than prose.
- **Boilerplate**: Keep every document minimal and to the point. Avoid
  unnecessary sections.
- **Future work**: File issues for future work items to track them, rather than
  including them in the design document.
