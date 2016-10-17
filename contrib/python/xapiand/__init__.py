# -*- coding: utf-8 -*-
#
# Copyright (C) 2015,2016 deipi.com LLC and contributors. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
from __future__ import absolute_import

import os
import itertools

try:
    from dfw.core.utils import json
    from dfw.core.utils.datastructures.nested import NestedDict
except ImportError:
    import json
    NestedDict = dict

__all__ = ['Xapiand']


try:
    import requests
except ImportError:
    raise ImportError("Xapiand requires the installation of the requests module.")


class Result(NestedDict):
    def __init__(self, *args, **kwargs):
        super(Result, self).__init__(*args, **kwargs)
        for k in list(self):
            if k[0] == '_':
                setattr(self, k, self.pop(k))


class Results(object):
    def __init__(self, meta, generator):
        for k, v in meta.get('_query', {}).items():
            if k[0] == '_':
                setattr(self, k, v)
        aggregations = NestedDict(meta.get('_aggregations', {}))
        setattr(self, 'aggregations', aggregations)
        self.generator = generator

    def __len__(self):
        return int(getattr(self, 'total_count', 0))

    def __iter__(self):
        return self

    def next(self):
        return Result(next(iter(self.generator)))
    __next__ = next


class DoesNotExist(Exception):
    pass

NA = object()


class Xapiand(object):

    """
    An object which manages connections to xapiand and acts as a
    go-between for API calls to it
    """

    session = requests.Session()
    session.mount('http://', requests.adapters.HTTPAdapter(pool_connections=100, pool_maxsize=100))
    _methods = dict(
        search=(session.get, True, 'results'),
        facets=(session.get, True, 'facets'),
        stats=(session.get, False, 'result'),
        get=(session.get, False, 'result'),
        delete=(session.delete, False, 'result'),
        head=(session.head, False, 'result'),
        post=(session.post, False, 'result'),
        put=(session.put, False, 'result'),
        patch=(session.patch, False, 'result'),
    )

    def __init__(self, ip='127.0.0.1', port=8880, commit=False, prefix=None):
        if ip and ':' in ip:
            ip, _, port = ip.partition(':')
        self.ip = ip
        self.port = port
        self.commit = commit
        self.prefix = prefix

    def _build_url(self, action_request, index, ip, port, nodename, id, body):
        if ip and ':' in ip:
            ip, _, port = ip.partition(':')
        if not ip:
            ip = self.ip
        if not port:
            port = self.port
        host = '{}:{}'.format(ip, port)

        prefix = '{}/'.format(self.prefix) if self.prefix else ''

        if not isinstance(index, (tuple, list)):
            index = [index]
        index = ','.join(prefix + i for i in index)

        nodename = '@{}'.format(nodename) if nodename else ''

        if id is None and action_request != 'post':
            action_request = '_{}/'.format(action_request)
        elif action_request == 'post':
            action_request = ''
        else:
            action_request = id

        url = 'http://{}/{}{}/{}'.format(host, index, nodename, action_request)

        return url

    def _send_request(self, action_request, index, ip=None, port=None, nodename=None, id=None, body=None, default=NA, **kwargs):
        """
        :arg action_request: Perform index, delete, serch, facets, stats, patch, head actions per request
        :arg query: Query to process on xapiand
        :arg index: index path
        :arg ip: address to connect to xapiand
        :arg port: port to connect to xapiand
        :arg nodename: Node name, if empty is assigned randomly
        :arg id: Document ID
        :arg body: File or dictionary with the body of the request
        """
        method, stream, key = self._methods[action_request]
        url = self._build_url(action_request, index, ip, port, nodename, id, body)

        params = kwargs.pop('params', None)
        if params is not None:
            kwargs['params'] = dict((k.replace('__', '.'), (v and 1 or 0) if isinstance(v, bool) else v) for k, v in params.items())

        stream = kwargs.pop('stream', stream)
        if stream is not None:
            kwargs['stream'] = stream

        if body is not None:
            if isinstance(body, dict):
                body = json.dumps(body)
            elif os.path.isfile(body):
                body = open(body, 'r')
            res = method(url, body, **kwargs)
        else:
            res = method(url, **kwargs)

        if res.status_code == 404 and action_request in ('patch', 'delete', 'get'):
            if default is NA:
                raise DoesNotExist
            return default
        else:
            res.raise_for_status()

        is_json = 'application/json' in res.headers.get('content-type', '')

        if stream:
            def results(lines, size=100):
                def feed():
                    feed.cache = []
                    feed.cache.extend(l for l in itertools.islice(lines, size) if l)
                feed()
                while feed.cache:
                    for line in feed.cache:
                        if is_json:
                            if line.lstrip().startswith(']'):
                                continue
                            elif line.rstrip().endswith('['):
                                line += ']}}'
                            else:
                                line = line.rstrip().rstrip(',')
                            yield json.loads(line)
                        else:
                            yield line
                    feed()
            results = results(res.iter_lines(delimiter='\n\n'))
            meta = next(results)
        else:
            results = [json.loads(res.content) if is_json else res.content]
            meta = {}

        results = Results(meta, results)
        if key == 'result':
            results = results.next()

        for k, v in res.headers.items():
            setattr(results, k.replace('-', '_'), v)

        return results

    def search(self, index, query=None, partial=None, terms=None, offset=None, limit=None, sort=None, facets=None, language=None, pretty=False, volatile=False, kwargs=None, **kw):
        kwargs = kwargs or {}
        kwargs.update(kw)
        kwargs['params'] = dict(
            pretty=pretty,
            volatile=volatile,
        )
        if query is not None:
            kwargs['params']['query'] = query
        if partial is not None:
            kwargs['params']['partial'] = partial
        if terms is not None:
            kwargs['params']['terms'] = terms
        if offset is not None:
            kwargs['params']['offset'] = offset
        if limit is not None:
            kwargs['params']['limit'] = limit
        if sort is not None:
            kwargs['params']['sort'] = sort
        if facets is not None:
            kwargs['params']['facets'] = facets
        if language is not None:
            kwargs['params']['language'] = language
        return self._send_request('search', index, **kwargs)

    def facets(self, index, query=None, partial=None, terms=None, offset=None, limit=None, sort=None, facets=None, language=None, pretty=False, volatile=False, kwargs=None, **kw):
        kwargs = kwargs or {}
        kwargs.update(kw)
        kwargs['params'] = dict(
            pretty=pretty,
            volatile=volatile,
        )
        if query is not None:
            kwargs['params']['query'] = query
        if partial is not None:
            kwargs['params']['partial'] = partial
        if terms is not None:
            kwargs['params']['terms'] = terms
        if offset is not None:
            kwargs['params']['offset'] = offset
        if limit is not None:
            kwargs['params']['limit'] = limit
        if sort is not None:
            kwargs['params']['sort'] = sort
        if facets is not None:
            kwargs['params']['facets'] = facets
        if language is not None:
            kwargs['params']['language'] = language
        return self._send_request('facets', index, **kwargs)

    def stats(self, index, pretty=False, kwargs=None):
        kwargs = kwargs or {}
        kwargs['params'] = dict(
            pretty=pretty,
        )
        return self._send_request('stats', index, **kwargs)

    def head(self, index, id, pretty=False, kwargs=None):
        kwargs = kwargs or {}
        kwargs['id'] = id
        kwargs['params'] = dict(
            pretty=pretty,
        )
        return self._send_request('head', index, **kwargs)

    def get(self, index, id, default=NA, pretty=False, volatile=False, kwargs=None):
        kwargs = kwargs or {}
        kwargs['id'] = id
        kwargs['params'] = dict(
            pretty=pretty,
            volatile=volatile,
        )
        kwargs['default'] = default
        return self._send_request('get', index, **kwargs)

    def delete(self, index, id, commit=None, pretty=False, kwargs=None):
        kwargs = kwargs or {}
        kwargs['id'] = id
        kwargs['params'] = dict(
            commit=self.commit if commit is None else commit,
            pretty=pretty,
        )
        return self._send_request('delete', index, **kwargs)

    def post(self, index, body, commit=None, pretty=False, kwargs=None):
        kwargs = kwargs or {}
        kwargs['body'] = body
        kwargs['params'] = dict(
            commit=self.commit if commit is None else commit,
            pretty=pretty,
        )
        return self._send_request('post', index, **kwargs)

    def put(self, index, body, id, commit=None, pretty=False, kwargs=None):
        kwargs = kwargs or {}
        kwargs['id'] = id
        kwargs['body'] = body
        kwargs['params'] = dict(
            commit=self.commit if commit is None else commit,
            pretty=pretty,
        )
        return self._send_request('put', index, **kwargs)
    index = put

    def patch(self, index, id, body, commit=None, pretty=False, kwargs=None):
        kwargs = kwargs or {}
        kwargs['id'] = id
        kwargs['body'] = body
        kwargs['params'] = dict(
            commit=self.commit if commit is None else commit,
            pretty=pretty,
        )
        return self._send_request('patch', index, **kwargs)


# TODO: Get settings for these from django.conf.settings:
XAPIAND_SANDBOX_PREFIX = 'sandbox'
XAPIAND_LIVE_PREFIX = 'live'
XAPIAND_HOST = '127.0.0.1'
XAPIAND_PORT = 8880
XAPIAND_COMMIT = False

live = Xapiand(ip=XAPIAND_HOST, port=XAPIAND_PORT, commit=XAPIAND_COMMIT, prefix=XAPIAND_LIVE_PREFIX)
sandbox = Xapiand(ip=XAPIAND_HOST, port=XAPIAND_PORT, commit=XAPIAND_COMMIT, prefix=XAPIAND_SANDBOX_PREFIX)
