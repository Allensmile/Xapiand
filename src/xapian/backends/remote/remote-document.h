/** @file remote-document.h
 * @brief A document read from a RemoteDatabase.
 */
/* Copyright (C) 2008,2011 Olly Betts
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef XAPIAN_INCLUDED_REMOTE_DOCUMENT_H
#define XAPIAN_INCLUDED_REMOTE_DOCUMENT_H

#include "xapian/backends/databaseinternal.h"
#include "xapian/backends/documentinternal.h"

/// A document read from a RemoteDatabase.
class RemoteDocument : public Xapian::Document::Internal {
    /// Don't allow assignment.
    void operator=(const RemoteDocument &);

    /// Don't allow copying.
    RemoteDocument(const RemoteDocument &);

    /// RemoteDocument::open_document() needs to call our private constructor.
    friend class RemoteDatabase;

    /** Private constructor - only called by RemoteDatabase::open_document().
     *
     *  @param values_	The values to set - passed by non-const reference, and
     *			may be modified by the call.
     */
    RemoteDocument(const Xapian::Database::Internal *db, Xapian::docid did_,
		   const string & data_,
		   btree::map<Xapian::valueno, string>&& values_)
	: Xapian::Document::Internal(db, did_, data_, std::move(values_)) {}

  protected:
    /** Implementation of virtual methods @{ */
    string fetch_value(Xapian::valueno slot) const;
    void fetch_all_values(btree::map<Xapian::valueno, string>& values_) const;
    string fetch_data() const;
    /** @} */
};

#endif // XAPIAN_INCLUDED_REMOTE_DOCUMENT_H
