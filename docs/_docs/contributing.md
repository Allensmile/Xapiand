---
title: Contributing
---

[Code of Conduct]: {{ '/docs/conduct' | relative_url }}
[GitHub Issue]: {{ site.repository }}/issues

The {{ site.name }} project welcomes, and depends, on contributions from developers and
users in the open source community. Contributions can be made in a number of
ways, a few examples are:

- Code patches via pull requests
- Documentation improvements
- Bug reports and patch reviews

Everyone interacting in the {{ site.name }} project's development, be it codebases, issue
trackers, chat rooms, and mailing lists is expected to follow the [Code of Conduct].

---

# The Road Ahead

This is a To Do list of features that are only partially implemented; but that
are planned to be supported by Xapiand some time soonish in order to get closer
to the final product:

* Multi-Partitioning and Distribution Strategies:
	* Social-Based Partitioning and Replication (SPAR <sup>[1](#footnote-1)</sup>).
	* Random Consistent Partitioning and Replication.

* Highly Available:
	* Automatic node operation rerouting.
	* Replicas exists to maximize high availability *and* data locality.
	* Read and Search operations performed on any of the replicas.
	* Reliable, asynchronous replication for long term persistency.


### Multi-Partitioning and Distribution Strategies

To achieve high availability, distribution of data and data locality, Xapiand
can partition, replicate and distribute indexes across several nodes using
any of the following partitioning strategies:


#### Social-Based Partitioning and Replication

* Horizontal scaling by distributing indexes among several nodes.
* Maximizes data locality ensuring related indexes are kept (or are directly
  available) in the same node.
* Minimizes network usage when accessing a set of related indexes.


#### Random Consistent Partitioning

* Horizontal scaling by distributing indexes among several nodes.


---


# Reporting an Issue

Please include as much detail as you can in the [GitHub Issue]. Let us know your
platform and {{ site.name }} version. If you get an error please include the full error
and traceback when available.

---

# Submitting Pull Requests

Once you are happy with your changes or you are ready for some feedback, push
it to your fork and send a pull request. For a change to be accepted it will
most likely need to have tests and documentation if it is a new feature.

---

# Hack the source code

If you feel adventurous, you can hack the source code of {{ site.name }}. You can find
information about how to build from the sources [here]({{ '/docs/building' | relative_url }}).

---

<sup><a id="footnote-1">1</a></sup> [The Little Engine(s) That Could: Scaling Online Social Networks.](http://ccr.sigcomm.org/online/files/p375.pdf){:target="_blank"}
