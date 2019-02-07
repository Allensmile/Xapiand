---
title: Queries
---

Queries within Xapiand are the mechanism by which documents are searched for
within a database. They can be a simple search for text-based terms or a search
based on the values assigned to documents, which can be combined using a number
of different methods to produce more complex queries.


## Simple Queries

The most basic query is a search for a single textual term. This will find all
documents in the database which have that term assigned to them.

For example, a search might be for the term "_banana_" assigned in the
"_favoriteFruit_" field and restricting the size of results to one result by
using the keyword `_limit`, which by default is set to 10:

{% capture req %}

```json
GET /bank/:search?pretty

{
  "_query": {
    "favoriteFruit": "banana"
  },
  "_limit": 1
}
```
{% endcapture %}
{% include curl.html req=req %}

When a query is executed, the result is a list of documents that match the
query, together with a **weight**, a **rank** and a **percent** for each which
indicates how good a match for the query that particular document is.


## Logical Operators

Each query produces a list of documents with a weight according to how good a
match each document is for that query. These queries can then be combined to
produce a more complex tree-like query structure, with the operators acting as
branches within the tree.

The most basic operators are the logical operators: `_or`, `_and` and `_not` -
these match documents in the following way:

* `_or`           - Finds documents which match any of the subqueries.
* `_and`          - Finds documents which match all of the subqueries.
* `_not`          - Finds documents which don't match any of the subqueries.
* `_and_not`      - Finds documents which match the first subquery A but
                    not subquery B.
* `_xor`          - Finds documents which are matched by subquery A or other or
                    subquery B, but not both.

Each operator produces a weight for each document it matches, which depends on
the weight of one or both subqueries in the following way:

* `_or`           - Matches documents with the sum of all weights of the subqueries.
* `_and`          - Matches documents with the sum of all weights of the subqueries.
* `_not`          - Finds documents which don't match any of the subqueries.
* `_and_not`      - Matches documents with the weight from subquery A only.

The following example finds _all_ bank accounts for which their account
holders are either _brown_ eyed _females_ or like _bananas_:

{% capture req %}

```json
GET /bank/:search?pretty

{
  "_query": {
    "_or": [
      {
        "_and": [
            { "gender": "female" },
            { "eyeColor": "brown" }
        ]
      },
      {
        "favoriteFruit": "banana"
      }
    ]
  }
}
```
{% endcapture %}
{% include curl.html req=req %}


## Maybe

In addition to the basic logical operators, there is an additional logical
operator `_and_maybe` which matches any document which matches A (whether or
not B matches). If only B matches, then `_and_maybe` doesn't match. For this
operator, the weight is the sum of the matching subqueries, so:

* `_and_maybe`    - Finds any document which matches the first element of the
                    array and whether or not matches the rest.

1. Documents which match A and B match with the weight of A+B
2. Documents which match A only match with weight of A

This allows you to state that you require some terms (A) and that other
terms (B) are useful but not required.

{% capture req %}

```json
GET /bank/:search?pretty

{
  "_query": {
    "_and_maybe": [
      {
        "_and": [
            { "gender": "female" },
            { "eyeColor": "brown" }
        ]
      },
      {
        "favoriteFruit": "banana"
      }
    ]
  }
}
```
{% endcapture %}
{% include curl.html req=req %}


## Filtering

A query can be filtered by another query. There are two ways to apply a filter
to a query depending whether you want to include or exclude documents:

* `_filter`       - Matches documents which match both subqueries, but the
                    weight is only taken from the left subquery (in other
                    respects it acts like `_and`.
* `_and_not`      - Matches documents which match the left subquery but don’t
                    match the right hand one (with weights coming from the left
                    subquery)


## Range Searches

The keyword `_range` matches documents where the given value is between the
given `_from` and `_to` fixed range (including both endpoints).

If you only use the keyword `_from` matches documents where the given value is
greater than or equal to a fixed value.

If you only use the keyword `_to` matches documents where the given value is
less than or equal a fixed value.

This example find _all_ bank accounts for which their account holders are
_females_ in the ages between 20 and 30:

{% capture req %}

```json
GET /bank/:search?pretty

{
  "_query": {
    "_and": [
      { "gender": "female" },
      {
        "age": {
          "_in": {
            "_range": {
              "_from": 20,
              "_to": 30
            }
          }
        }
      }
    ]
  }
}
```
{% endcapture %}
{% include curl.html req=req %}


## Near

Two additional operators that are commonly used are `_near`, which finds terms
within 10 words of each other in the current document, behaving like `_and`
with regard to weights, so that:

* Documents which match A within 10 words of B are matched, with weight of A+B

{: .note .unreleased}
**_Unimplemented Feature!_**<br>
This feature hasn't yet been implemented...
[Pull requests are welcome!]({{ site.repository }}/pulls)

{% capture req %}

```json
GET /bank/:search?pretty

{
  "_query": {
    "_personality": {
        "_value": "adventurous",
        "_near": "assures"
    }
  }
}
```
{% endcapture %}
{% include curl.html req=req %}


## Phrase

The `_phrase` operator allows for searching for a specific phrase and returns
only matches where all terms appear in the document, in the correct order,
giving a weight of the sum of each term. For example:

* Documents which match A followed by B followed by C gives a weight of A+B+C

{: .note .unreleased}
**_Unimplemented Feature!_**<br>
This feature hasn't yet been implemented...
[Pull requests are welcome!]({{ site.repository }}/pulls)

{% capture req %}

```json
GET /bank/:search?pretty

{
  "_query": {
    "_personality": {
        "_phrase": "All in all"
    }
  }
}
```
{% endcapture %}
{% include curl.html req=req %}

## Additional Operators

* `_elite_set`    - Pick the best N subqueries and combine with `_or`.
* `_max`          - Pick the maximum weight of any subquery.
* `_wildcard`     - Wildcard expansion.

<!--
* `_scale_weight` -
* `_synonym`      -
 -->
