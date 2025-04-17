# Project Governance

The Valkey project is managed by a Technical Steering Committee (TSC) composed of the maintainers of the Valkey repository.
The Valkey project includes all of the current and future repositories under the Valkey-io organization.
Committers are defined as individuals with write access to the code within a repository.
Maintainers are defined as individuals with full access to a repository and own its governance.
Both maintainers and committers should be clearly listed in the MAINTAINERS.md file in a given projects repository.
Maintainers of other repositories within the Valkey project are not members of the TSC unless explicitly added.

## Technical Steering Committee

The TSC is responsible for oversight of all technical, project, approval, and policy matters for Valkey.

The TSC members are listed in the [MAINTAINERS.md](MAINTAINERS.md) file in the Valkey repository.
Maintainers (and accordingly, TSC members) may be added or removed by no less than 2/3 affirmative vote of the current TSC.
The TSC shall appoint a Chair responsible for organizing TSC meetings.
If the TSC Chair is removed from the TSC (or the Chair steps down from that role), it is the responsibility of the TSC to appoint a new Chair.
The TSC can amend this governance document by no less than a 2/3 affirmative vote.

The TSC may, at its discretion, add or remove members who are not maintainers of the main Valkey repository.
The TSC may, at its discretion, add or remove maintainers from other repositories within the Valkey project.

## Voting

The TSC shall strive for all decisions to be made by consensus.
While explicit agreement of the entire TSC is preferred, it is not required for consensus.
Rather, the TSC shall determine consensus based on their good faith consideration of a number of factors, including the dominant view of the TSC and nature of support and objections.
The TSC shall document evidence of consensus in accordance with these requirements.
If consensus cannot be reached, the TSC shall make the decision by a vote.

A vote shall also be called when an issue or pull request is marked as a major decision, which are decisions that have a significant impact on the Valkey architecture or design.
Examples of major decisions:
    * Fundamental changes to the Valkey core datastructures
    * Adding a new data structure or API
    * Changes that affect backward compatibility
    * New user visible fields that need to be maintained
    * Modifications to the TSC or other governance documents
    * Adding members to other roles within the Valkey project
    * Delegation of maintainership for projects to other groups or individuals
    * Adding or removing a new external library such as a client
    or module to the project.

Any member of the TSC can call a vote with reasonable notice to the TSC, setting out a discussion period and a separate voting period.
Any discussion may be conducted in person or electronically by text, voice, or video.
The discussion shall be open to the public, with the notable exception of discussions involving embargoed security issues or the addition or removal of maintainers, which will be private.
In any vote, each voting TSC member will have one vote.
The TSC shall give at least two weeks for all members to submit their vote.
Except as specifically noted elsewhere in this document, decisions by vote require a simple majority vote of all voting members.
It is the responsibility of the TSC chair to help facilitate the voting process as needed to make sure it completes within the voting period.

## Termination of Membership

A maintainer's access (and accordingly, their position on the TSC) will be removed if any of the following occur:

* Resignation: Written notice of resignation to the TSC.
* TSC Vote: 2/3 affirmative vote of the TSC to remove a member
* Unreachable Member: If a member is unresponsive for more than six months, the remaining active members of the TSC may vote to remove the unreachable member by a simple majority.

## Technical direction for other Valkey projects

The TSC may delegate decision making for other projects within the Valkey organization to the maintainers responsible for those projects.
Delegation of decision making for a project is considered a major decision, and shall happen with an explicit vote.
Projects within the Valkey organization must indicate the individuals with commit permissions by updating the MAINTAINERS.md within their repositories.

The TSC may, at its discretion, overrule the decisions made by other projects within the Valkey organization, although they should show restraint in doing so.

## License of this document

This document may be used, modified, and/or distributed under the terms of the
[Creative Commons Attribution 4.0 International (CC-BY) license](https://creativecommons.org/licenses/by/4.0/legalcode).
