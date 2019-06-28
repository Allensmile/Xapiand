---
title: Tests
permalink: /tests/
---

* [**_Check Datatypes_**](dataTypes/check-data-types)
* [**_Check DateType_**](dataTypes/date-type)
* [**_Global terms_**](globalTerms/global-terms)
* [**_Namespace_**](namespace/namespace)
* [**_Null type_**](nullType/null-type)
* [**_Replace Value by Null_**](nullType/replace-null)
* [**_Ignore Comments_**](comments/ignore-comments)
* [**_Create Schema_**](create-schema)
* [**_Create Schema With UUID Field_**](uuid-field)
* [**_Create Schema With UUID Field_**](create.schema.uuid)
* [**_Arrays_**](dataStructures/arrays)
* [**_Objects_**](dataStructures/objects)


### Reserved Subfield

{% comment %}
```json
PUT /test/reserved_subfield/doc

{
  "name": {
    "_type": "text",
    "_reserved": "this is reserved, should fail"
  }
}
```
---
description: Index Misuse Reserved Subfield
---

```js
pm.test("Response is success", function() {
  pm.expect(pm.response.code).to.equal(400);
});
```
{% endcomment %}
