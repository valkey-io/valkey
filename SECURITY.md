## Reporting a Vulnerability

If you believe you've discovered a security vulnerability, please contact the Valkey team at security@lists.valkey.io.
Please *DO NOT* create an issue.
We follow a responsible disclosure procedure, so depending on the severity of the issue we may notify Valkey vendors about the issue before releasing it publicly.
If you would like to be added to our list of vendors, please reach out to the Valkey team at maintainers@lists.valkey.io.

## Severity and CVEs

We score vulnerabilities with CVSS 4.0, so you can estimate the severity before you contact the Valkey team: https://www.first.org/cvss/calculator/4.0
We assign a CVE and follow our responsible disclosure procedure for issues that score High (7.0 or above).
Lower scores are fixed through a normal public pull request and noted in the release.
Please still report those privately and we will tell you if we plan to fix it in public.
When you report, please include the Valkey version, the relevant configuration, a reproducer if you have one, and the name you would like us to credit.

## Cluster Bus

The cluster bus port only opens when `cluster-enabled yes` is set.
We treat it as a trusted interface for cluster nodes, reachable only from other cluster nodes, for example behind a firewall or on a private network.
We fix cluster bus bugs in public and assign a CVE only if the report shows remote code execution.
We score them as Attack Vector Adjacent and Privileges Required None, unless `tls-cluster` is enabled or the port is reachable from outside the intended network.
