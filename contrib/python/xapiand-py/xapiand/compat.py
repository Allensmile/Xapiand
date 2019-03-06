# Copyright (c) 2019 Dubalu LLC
# Copyright (c) 2017 Elasticsearch
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to you under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


import sys

PY2 = sys.version_info[0] == 2

if PY2:
    text_type = unicode
    binary_type = str
    string_types = basestring,
    from urllib import quote_plus, urlencode, unquote
    from urlparse import  urlparse
    from itertools import imap as map
    from Queue import Queue
else:
    text_type = str
    binary_type = bytes
    string_types = str, bytes
    from urllib.parse import quote_plus, urlencode, urlparse, unquote
    map = map
    from queue import Queue
