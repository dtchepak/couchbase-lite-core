/**
 * Copyright (c) 2017 Couchbase, Inc. All rights reserved.
 * <p>
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of the License at
 * <p>
 * http://www.apache.org/licenses/LICENSE-2.0
 * <p>
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */
package com.couchbase.litecore;

import org.junit.Test;

import java.io.File;
import java.util.Arrays;
import java.util.List;

import static com.couchbase.litecore.Constants.C4IndexType.kC4FullTextIndex;
import static com.couchbase.litecore.Constants.C4IndexType.kC4ValueIndex;
import static com.couchbase.litecore.Constants.C4RevisionFlags.kRevDeleted;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.fail;

public class C4QueryTest extends C4QueryBaseTest {
    static final String LOG_TAG = C4QueryTest.class.getSimpleName();

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    @Override
    public void setUp() throws Exception {
        super.setUp();
        Utils.copyAssets("names_100.json", context.getFilesDir());
        importJSONLines(new File(context.getFilesDir(), "names_100.json"));
    }

    @Override
    public void tearDown() throws Exception {
        if (query != null) {
            query.free();
            query = null;
        }
        super.tearDown();
    }

    //-------------------------------------------------------------------------
    // tests
    //-------------------------------------------------------------------------

    // -- Query parser error messages
    @Test
    public void testDatabaseErrorMessages() {
        try {
            new C4Query(db, "[\"=\"]");
            fail();
        } catch (LiteCoreException e) {
            assertEquals(C4ErrorDomain.LiteCoreDomain, e.domain);
            assertEquals(LiteCoreError.kC4ErrorInvalidQuery, e.code);
        }
    }

    // - DB Query
    @Test
    public void testDBQuery() throws LiteCoreException {
        compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
        assertEquals(Arrays.asList("0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"), run());
        assertEquals(Arrays.asList("0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"), run(1, 8));
        assertEquals(Arrays.asList("0000015", "0000036", "0000043", "0000053"), run(1, 4));

        compile(json5("['AND', ['=', ['array_count()', ['.', 'contact', 'phone']], 2],['=', ['.', 'gender'], 'male']]"));
        assertEquals(Arrays.asList("0000002", "0000014", "0000017", "0000027", "0000031", "0000033", "0000038", "0000039", "0000045", "0000047",
                "0000049", "0000056", "0000063", "0000065", "0000075", "0000082", "0000089", "0000094", "0000097"), run());

        // MISSING means no value is present (at that array index or dict key)
        compile(json5("['IS', ['.', 'contact', 'phone', [0]], ['MISSING']]"));
        assertEquals(Arrays.asList("0000004", "0000006", "0000008", "0000015"), run(0, 4));

        // ...wherease null is a JSON null value
        compile(json5("['IS', ['.', 'contact', 'phone', [0]], null]"));
        assertEquals(Arrays.asList(), run(0, 4));
    }

    // - DB Query sorted
    @Test
    public void testDBQuerySorted() throws LiteCoreException {
        compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"),
                json5("[['.', 'name', 'last']]"));
        assertEquals(Arrays.asList("0000015", "0000036", "0000072", "0000043", "0000001", "0000064", "0000073", "0000053"), run());
    }

    // - DB Query bindings
    @Test
    public void testDBQueryBindings() throws LiteCoreException {
        compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$', 1]]"));
        assertEquals(Arrays.asList("0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"), run(0, Long.MAX_VALUE, "{\"1\": \"CA\"}"));

        compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$', 'state']]"));
        assertEquals(Arrays.asList("0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"), run(0, Long.MAX_VALUE, "{\"state\": \"CA\"}"));
    }

    // - DB Query ANY
    @Test
    public void testDBQueryANY() throws LiteCoreException {
        compile(json5("['ANY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'climbing']]"));
        assertEquals(Arrays.asList("0000017", "0000021", "0000023", "0000045", "0000060"), run());

        // This EVERY query has lots of results because every empty `likes` array matches it
        compile(json5("['EVERY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'taxes']]"));
        List<String> result = run();
        assertEquals(42, result.size());
        assertEquals("0000007", result.get(0));

        // Changing the op to ANY AND EVERY returns no results
        compile(json5("['ANY AND EVERY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'taxes']]"));
        assertEquals(Arrays.asList(), run());

        // Look for people where every like contains an L:
        compile(json5("['ANY AND EVERY', 'like', ['.', 'likes'], ['LIKE', ['?', 'like'], '%l%']]"));
        assertEquals(Arrays.asList("0000017", "0000027", "0000060", "0000068"), run());
    }

    // - DB Query expression index
    @Test
    public void testDBQueryExpressionIndex() throws LiteCoreException {
        db.createIndex(json5("[['length()', ['.name.first']]]"), kC4ValueIndex, null, true);
        compile(json5("['=', ['length()', ['.name.first']], 9]"));
        assertEquals(Arrays.asList("0000015", "0000099"), run(0, Long.MAX_VALUE));
    }

    // - Delete indexed doc
    @Test
    public void testDeleteIndexedDoc() throws LiteCoreException {
        // Create the same index as the above test:
        db.createIndex(json5("[['length()', ['.name.first']]]"), kC4ValueIndex, null, true);

        // Delete doc "0000015":
        {
            boolean commit = false;
            db.beginTransaction();
            try {
                Document doc = db.getDocument("0000015", true);
                assertNotNull(doc);
                String[] history = {doc.getRevID()};
                Document updatedDoc = db.put(doc.getDocID(), (byte[]) null, null, false, false, history, kRevDeleted, true, 0);
                assertNotNull(updatedDoc);
                doc.free();
                updatedDoc.free();
                commit = true;
            } finally {
                db.endTransaction(commit);
            }

        }

        // Now run a query that would have returned the deleted doc, if it weren't deleted:
        compile(json5("['=', ['length()', ['.name.first']], 9]"));
        assertEquals(Arrays.asList("0000099"), run(0, Long.MAX_VALUE));
    }


    // - Full-text query
    @Test
    public void testFullTextQuery() throws LiteCoreException {
        db.createIndex("[[\".contact.address.street\"]]", kC4FullTextIndex, null, true);
        compile(json5("['MATCH', ['.', 'contact', 'address', 'street'], 'Hwy']"));
        assertEquals(Arrays.asList("0000013", "0000015", "0000043", "0000044", "0000052"), run(0, Long.MAX_VALUE));
    }

    // - DB Query WHAT
    @Test
    public void testDBQueryWHAT() throws LiteCoreException {
        List<String> expectedFirst = Arrays.asList("Cleveland", "Georgetta", "Margaretta");
        List<String> expectedLast = Arrays.asList("Bejcek", "Kolding", "Ogwynn");
        compile(json5("{WHAT: ['.name.first', '.name.last'], WHERE: ['>=', ['length()', ['.name.first']], 9],ORDER_BY: [['.name.first']]}"));

        C4QueryEnumerator e = query.run(new C4QueryOptions(), null);
        assertNotNull(e);
        int i = 0;
        while (e.next()) {
            byte[] columns = e.getCustomColumns();
            assertEquals(getColumn(columns, 0), expectedFirst.get(i));
            assertEquals(getColumn(columns, 1), expectedLast.get(i));
            i++;
        }
        e.free();
        assertEquals(3, i);
    }

    // - DB Query Aggregate
    @Test
    public void testDBQueryAggregate() throws LiteCoreException {
        compile(json5("{WHAT: [['min()', ['.name.last']], ['max()', ['.name.last']]]}"));

        C4QueryEnumerator e = query.run(new C4QueryOptions(), null);
        assertNotNull(e);
        int i = 0;
        while (e.next()) {
            byte[] columns = e.getCustomColumns();
            assertEquals(getColumn(columns, 0), "Aerni");
            assertEquals(getColumn(columns, 1), "Zirk");
            i++;
        }
        e.free();
        assertEquals(1, i);
    }

    // - DB Query Grouped
    @Test
    public void testDBQueryGrouped() throws LiteCoreException {

        final List<String> expectedState = Arrays.asList("AL", "AR", "AZ", "CA");
        final List<String> expectedMin = Arrays.asList("Laidlaw", "Okorududu", "Kinatyan", "Bejcek");
        final List<String> expectedMax = Arrays.asList("Mulneix", "Schmith", "Kinatyan", "Visnic");
        final int expectedRowCount = 42;

        compile(json5("{WHAT: [['.contact.address.state'], ['min()', ['.name.last']], ['max()', ['.name.last']]],GROUP_BY: [['.contact.address.state']]}"));

        C4QueryEnumerator e = query.run(new C4QueryOptions(), null);
        assertNotNull(e);
        int i = 0;
        while (e.next()) {
            byte[] columns = e.getCustomColumns();
            if (i < expectedState.size()) {
                assertEquals(getColumn(columns, 0), expectedState.get(i));
                assertEquals(getColumn(columns, 1), expectedMin.get(i));
                assertEquals(getColumn(columns, 2), expectedMax.get(i));
            }
            i++;
        }
        e.free();
        assertEquals(expectedRowCount, i);
    }


}