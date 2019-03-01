---
title: Scripting
---

The scripting module enables you to use scripts to evaluate custom expressions.
For example, you could use a script to return "script fields" as part of a
search request or evaluate a custom score for a query.

The scripting language currently supported is
[ChaiScript](http://chaiscript.com){:target="_blank"}.

For example, the following script is used while adding/updating a given document
to atomically increment a "serial" number field:

{% capture req %}

```json
PUT /customer/1?pretty

{
  "serial": 1,
  "_script": "doc.serial = (old ? old.serial + 1 : doc.serial)"
}
```
{% endcapture %}
{% include curl.html req=req %}

{: .note .tip }
**_Dot Access Notation_**<br>
When accessing document objects in the scripts, you can either use _dot access_
notation as exeplified above (i.e. `doc.serial`) or _array call_ notation
(i.e. `doc["serial"]`).


## How to Use Scripts

Wherever scripting is supported in the Xapiand API, the structure for scripts
follows the same patterns:

```json
"_script" {
  "_type": "script",
  "_value": "<script_name|script_body>",
  ( "_name": "<script_name>", )?
  ( "_params": <params>, )?
}
```

Or, for short:

```json
"_script": "<script_name|script_body>"
```


{: .note .tip }
**_Prefer Parameters_**<br>
The first time Xapiand sees a new script, it compiles it and stores the compiled
version in a cache. Compilation can be a **heavy** process, so try using
_Named Scripts_ and _Variables_.


### Variables

If you need to pass variables into the script, you should pass them in as named
parameters instead of hard-coding values into the script itself. For example, if
you want to be able to multiply a field value by different multipliers, don't
hard-code the multiplier into the script:

{% capture req %}

```json
PUT /customer/1?pretty

{
  "multiplied_field": 7,
  "_script": {
    "_type": "script",
    "_value": "doc.multiplied_field *= 2"
  }
}
```
{% endcapture %}
{% include curl.html req=req %}

Instead, pass it in as a named parameter:

{% capture req %}

```json
PUT /customer/1?pretty

{
  "multiplied_field": 7,
  "_script": {
    "_type": "script",
    "_value": "doc.multiplied_field *= multiplier",
    "_params": {
      "multiplier": 2
    }
  }
}
```
{% endcapture %}
{% include curl.html req=req %}

The first version has to be recompiled every time the multiplier changes. The
second version is only compiled once.


### Script Caching

All scripts are cached by default so that they only need to be recompiled when
updates occur. By default, the cache size is 100 and scripts do not have a
time-based expiration.
