//
//  CBForestVersions.h
//  CBForest
//
//  Created by Jens Alfke on 12/3/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestVersions.h"
#import "CBForestPrivate.h"
#import "rev_tree.h"
#import "option.h"


#define kDefaultMaxDepth 100


@interface CBForestVersions ()
@property (readwrite) CBForestVersionsFlags flags;
@property (readwrite) NSString* revID;
@end


@implementation CBForestVersions
{
    CBForestDocument* _doc;
    NSData* _rawTree;
    RevTree* _tree;
    BOOL _changed;
    NSMutableArray* _insertedData;
}


@synthesize maxDepth=_maxDepth, flags=_flags, revID=_revID;


- (id) initWithDB: (CBForestDB*)store docID: (NSString*)docID {
    self = [super initWithDB: store docID: docID];
    if (self) {
        _maxDepth = kDefaultMaxDepth;
        _tree = RevTreeNew(1);
    }
    return self;
}

- (id) initWithDB: (CBForestDB*)store
                info: (const fdb_doc*)info
              offset: (uint64_t)bodyOffset
{
    self = [super initWithDB: store info: info offset: bodyOffset];
    if (self) {
        _maxDepth = kDefaultMaxDepth;
    }
    return self;
}


- (BOOL) readTree: (NSError**)outError {
    _rawTree = [self readBody: outError];
    if (!_rawTree)
        return NO;
    NSAssert(self.bodyFileOffset > 0, @"Body offset unknown");
    RevTree* tree = RevTreeDecode(DataToBuf(_rawTree), 1, self.sequence, self.bodyFileOffset);
    if (!tree) {
        if (outError)
            *outError = [NSError errorWithDomain: CBForestErrorDomain
                                            code: kCBForestErrorDataCorrupt
                                        userInfo: nil];
        return NO;
    }
    RevTreeFree(_tree);
    _tree = tree;
    return YES;
}


- (void) dealloc {
    RevTreeFree(_tree);
}


static CBForestVersionsFlags flagsFromMeta(const fdb_doc* docinfo) {
    if (docinfo->metalen == 0)
        return 0;
    return ((UInt8*)docinfo->meta)[0];
}


- (BOOL) reloadMeta:(NSError *__autoreleasing *)outError {
    if (![super reloadMeta: outError])
        return NO;
    // Decode flags & revID from metadata:
    NSData* meta = self.metadata;
    if (meta.length >= 1) {
        const void* metabytes = meta.bytes;
        CBForestVersionsFlags flags = *(uint8_t*)metabytes;
        NSString* revID = ExpandRevID((sized_buf){(void*)metabytes+1, meta.length-1});
        if (flags != _flags || ![revID isEqualToString: _revID]) {
            self.flags = flags;
            self.revID = revID;
            if (![self readTree: outError])
                return NO;
        }
    } else {
        self.flags = 0;
        self.revID = nil;
    }
    return YES;
}


- (BOOL) save: (NSError**)outError {
    if (!_changed)
        return YES;

    RevTreePrune(_tree, _maxDepth);
    sized_buf encoded = RevTreeEncode(_tree);

    // Encode flags & revID into metadata:
    NSMutableData* metadata = [NSMutableData dataWithLength: 1 + _revID.length];
    void* bytes = metadata.mutableBytes;
    *(uint8_t*)bytes = _flags;
    sized_buf dstRev = {bytes+1, metadata.length-1};
    RevIDCompact(StringToBuf(_revID), &dstRev);
    metadata.length = 1 + dstRev.size;

    BOOL ok = [self writeBody: BufToData(encoded.buf, encoded.size)
                     metadata: metadata
                        error: outError];
    free(encoded.buf);
    if (ok)
        _changed = NO;
    return ok;
}


- (NSUInteger) revisionCount {
    return RevTreeGetCount(_tree);
}


static NSData* dataForNode(fdb_handle* db, const RevNode* node, NSError** outError) {
    if (outError)
        *outError = nil;
    if (!node)
        return nil;
    NSData* result = nil;
    if (node->data.size > 0) {
        result = BufToData(node->data.buf, node->data.size);
    }
#ifdef REVTREE_USES_FILE_OFFSETS
    else if (node->oldBodyOffset > 0) {
        // Look up old document from the saved oldBodyOffset:
        fdb_doc doc = {.seqnum = node->sequence};
        if (!Check(fdb_get_byoffset(db, &doc, node->oldBodyOffset), outError))
            return nil; // This will happen if the old doc body was lost by compaction.
        RevTree* oldTree = RevTreeDecode((sized_buf){doc.body, doc.bodylen}, 0, 0, 0);
        if (oldTree) {
            // Now look up the revision, which still has a body in this old doc:
            const RevNode* oldNode = RevTreeFindNode(oldTree, node->revID);
            if (oldNode && oldNode->data.buf)
                result = BufToData(oldNode->data.buf, oldNode->data.size);
            RevTreeFree(oldTree);
        }
        free(doc.body);
        if (!result && outError)
            *outError = [NSError errorWithDomain: CBForestErrorDomain
                                            code: kCBForestErrorDataCorrupt
                                        userInfo: nil];
    }
#endif
    return result;
}

- (const RevNode*) nodeWithID: (NSString*)revID {
    return RevTreeFindNode(_tree, CompactRevIDToBuf(revID));
}

- (NSData*) currentRevisionData {
    RevTreeSort(_tree);
    return dataForNode(self.db.handle, RevTreeGetNode(_tree, 0), NULL);
}

- (NSData*) dataOfRevision: (NSString*)revID {
    return dataForNode(self.db.handle, [self nodeWithID: revID], NULL);
}

- (BOOL) isRevisionDeleted: (NSString*)revID {
    const RevNode* node = [self nodeWithID: revID];
    return node && (node->flags & kRevNodeIsDeleted);
}

- (BOOL) hasRevision: (NSString*)revID {
    return [self nodeWithID: revID] != NULL;
}

- (BOOL) hasRevision: (NSString*)revID isLeaf:(BOOL *)outIsLeaf {
    const RevNode* node = [self nodeWithID: revID];
    if (!node)
        return NO;
    if (outIsLeaf)
        *outIsLeaf = (node->flags & kRevNodeIsLeaf) != 0;
    return YES;
}

static BOOL nodeIsActive(const RevNode* node) {
    return node && (node->flags & kRevNodeIsLeaf) && !(node->flags & kRevNodeIsDeleted);
}

- (BOOL) hasConflicts {
    return RevTreeHasConflict(_tree);
}

- (NSArray*) currentRevisionIDs {
    RevTreeSort(_tree);
    NSMutableArray* conflicts = [NSMutableArray array];
    for (unsigned i = 0; YES; ++i) {
        const RevNode* node = RevTreeGetNode(_tree, i);
        if (!nodeIsActive(node))
            break;
        [conflicts addObject: ExpandRevID(node->revID)];
    }
    return conflicts;
}

- (NSArray*) historyOfRevision: (NSString*)revID {
    RevTreeSort(_tree);
    const RevNode* node;
    if (revID)
        node = [self nodeWithID: revID];
    else
        node = RevTreeGetNode(_tree, 0);
    
    NSMutableArray* history = [NSMutableArray array];
    while (node) {
        [history addObject: ExpandRevID(node->revID)];
        node = RevTreeGetNode(_tree, node->parentIndex);
    }
    return history;
}


#pragma mark - INSERTION:


// Use this to get a sized_buf for any data that's going to be added to the RevTree.
// It adds a reference to the NSData so it won't be dealloced and invalidate the sized_buf.
- (sized_buf) rememberData: (NSData*)data {
    if (!data)
        return (sized_buf){NULL, 0};
    data = [data copy]; // in case it's mutable
    if (!_insertedData)
        _insertedData = [NSMutableArray array];
    [_insertedData addObject: data];
    return DataToBuf(data);
}


// Update the flags and current revision ID.
- (void) updateAfterInsert {
    RevTreeSort(_tree);
    const RevNode* curNode = RevTreeGetCurrentNode(_tree);
    CBForestVersionsFlags flags = 0;
    if (curNode->flags & kRevNodeIsDeleted)
        flags |= kCBForestDocDeleted;
    if (RevTreeHasConflict(_tree))
        flags |= kCBForestDocConflicted;
    self.flags = flags;
    self.revID = ExpandRevID(curNode->revID);
    _changed = YES;
}


- (BOOL) addRevision: (NSData*)data
            deletion: (BOOL)deletion
              withID: (NSString*)revID
            parentID: (NSString*)parentRevID
       allowConflict: (BOOL)allowConflict
{
    if (!RevTreeInsert(&_tree,
                       [self rememberData: CompactRevID(revID)],
                       [self rememberData: data],
                       deletion,
                       CompactRevIDToBuf(parentRevID),
                       allowConflict))
        return NO;
    [self updateAfterInsert];
    return YES;
}


- (NSInteger) addRevision: (NSData *)data
                 deletion: (BOOL)deletion
                  history: (NSArray*)history // history[0] is new rev's ID
{
    NSUInteger historyCount = history.count;
    sized_buf *historyBufs = malloc(historyCount * sizeof(sized_buf));
    if (!historyBufs)
        return -1;
    for (NSUInteger i=0; i<historyCount; i++)
        historyBufs[i] = [self rememberData: CompactRevID(history[i])];
    int numAdded = RevTreeInsertWithHistory(&_tree, historyBufs, (unsigned)historyCount,
                                            [self rememberData: data], deletion);
    free(historyBufs);
    if (numAdded > 0)
        [self updateAfterInsert];
    return numAdded;
}


+ (BOOL) docInfo: (const fdb_doc*)docInfo
  matchesOptions: (const CBForestEnumerationOptions*)options
{
    if (!options || !options->includeDeleted) {
        if (flagsFromMeta(docInfo) & kCBForestDocDeleted)
            return NO;
    }
    if (options && options->onlyConflicts) {
        if (!(flagsFromMeta(docInfo) & kCBForestDocConflicted))
            return NO;
    }
    return YES;
}


- (NSString*) dump {
    NSMutableString* out = [NSMutableString stringWithFormat: @"Doc %@ / %@\n",
                            self.docID, self.revID];
    RevTreeSort(_tree);
    for (int i=0; i<RevTreeGetCount(_tree); i++) {
        const RevNode* node = RevTreeGetNode(_tree, i);
        [out appendFormat: @"  #%2d: %@", i, ExpandRevID(node->revID)];
        if (node->flags & kRevNodeIsDeleted)
            [out appendString: @" (DEL)"];
        if (node->flags & kRevNodeIsLeaf)
            [out appendString: @" (leaf)"];
        if (node->flags & kRevNodeIsNew)
            [out appendString: @" (new)"];
        if (node->parentIndex != kRevNodeParentIndexNone)
            [out appendFormat: @" ^%d", node->parentIndex];
        [out appendFormat: @"\n"];
    }
    return out;
}


@end
