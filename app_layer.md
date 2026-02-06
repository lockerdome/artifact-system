

# artifact-server layer above LakeFS

LakeFS provides transactions, versioning and storage. This leaves many application-specific pieces of functionality.
Specifically, the artifact server msut support artifact schemas (and types), application-specific logic, indexes,
groups, resolving merge conflicts, providing metadata for an artifact type, caching in redis, and many other things.

## Necessary features for launch

1. Integration with LakeFS
1. Indexes for looking up artifacts (and ability to resolve conflicts)
1. Artifact Type registry (with version and schema support)

## Additional features which should be planned for

1. Caching objects in redis
1. Some form of validation of permissions
1. Triggers
1. Sharding of indexes for when they get extremely large (think "get me all artifacts of type X")
1. Migrating indexes (adding or removing indexes)
1. Schema validation for artifacts
1. Supporting pulling of "part" of an artifact

## Integration with LakeFS

LakeFS can be set up with the providion.sh script. This will create a MIG, Postgres database, Load Balancer, Image,
Instance Template, Health Check, Spark vacuum Service, VPC, and a few other odds and ends which enable a highly
available LakeFS cluster. Note that the script uses two zones with the MIG to ensure high availability and utilizes
the HA version of Postgres. The costs are low enough that this seems like a worthwhile choice. The script will also
create a bucket in Cloud Storage called lakefs-data-<project-id>. You will probably want to set the PROJECT_ID before
running the provisioning script and check that the IP ranges will work with our broader system.

The secret needed to login with the admin account is saved in the secret manager as lakefs-secret-access-key and
the access key id is static and available (or changeable) by updating the ACCESS_KEY_ID value in the provision script.
With these, you will be able to login to the LakeFS admin portal, but, due to how locked down everything is, you will
need to tunnel using IAP and SSH then use that connection to locally bind a port which gets forwarded along to one of
the instances for communicating with the http server. Assuming that the provision script runs successfully, it will
provide instructions at the end.

I would suggest that we create a repo in LakeFS for the artifacts. It is important to note that if we need to have
transactions across more than artifacts, all those will need to be in the same repo of LakeFS. It does not support
cross-repo transactions. Additionally, it is important to note that the Spark cleanup process will remove commits which
are not recent and not directly pinned by a tag or referenced as the head of a branch. We could choose to change this
behavior, but, the default they recommend will "vacuum" anything older than the time window and which is not referenced.

### Conflicts

LakeFS is designed to be general purpose and, as such, it does not assume that it knows how to merge when objects
diverge in two branches. This is unlikely to cause a surprise when updating an artifact, however, because we will be
using objects to store the state of indexes, those will be very likely to have conflicts. There are two tools available
here: first, we can use pre-merge hooks; I don't recommend those for conflict resolution and, given that we are adding
our own layer on top of LakeFS, I suspect that this feature will be unhelpful for us. The second tool, and the one I
recommend, is to handle the conflict after trying to merge. To do this, simply create logic that when a transaction
attempts to commit, if the LakeFS server returns with a Conflict, read through the conflicts, fix them with application
logic and then retry the merge. Repeat this until it merges successfully or too many tries are reached. The latter
should not really ever occur in production with our current levels of concurrency.

## Indexes for looking up artifacts

We have discussed having tags or groups which are both essentially higher level abstractions around the idea of an
index. While what we index on will likely be at least partially driven by the particular artifact type, the underlying
way we store the artifacts (or singular artifact when an index is unique) which are present in each index will likely
be uniform. I would suggest a similar approach to what we use in ReactDB. Of note, however, any changes to an index will
conflict, so merging will require doing a diff between the common base, current head we are merging into and the current
head of what we are merging for each conflicting index. (See Conflicts above) In other words, we will need to read the
changes and reconstruct what the edits are for each diverging branch and then apply all those edits. If the index is a
unique index, we would obviously require that any final version contains zero or one item. Empty indexes should
probably simply be encoded by the lack of an object.

### How we scope artifacts vs indexes vs types

In order to support indexes, we need to ensure that they will never overlap with artifacts, but they must be in the same
path structure where we store all objects. I suggest something simple like this: use artifacts/<artifact id> for the
artifacts themselves and indexes/<index name>/<index key> for the indexes. We could also have types/<artifact type name>
be where we store the artifact type information. We could also store types/<artifact type name>/<version> when we wish
to version these. I would strongly recommend against making any of these details transparent to end-users and would
treat the actual paths of objects in cloud storage as a private implimentation detail.

## Artifact Type registry (with version and schema support)

We likely want to represent the artifact types as tied to the current state of the repo. This is helpful because we can
create migration transactions and detect when merging if something needs to be corrected before merging. For example, if
we add an index, we can create a new branch with the index and have it populate that index for all the existing data. On
merge, it will look at the branch and detect any changes since it was forked which woudl require adjustments to that
index and only merge where everything lines up. We can use a similar process when removing an index. If we want to force
all artifacts of a particular version of artifact schema to migrate, we can also create this migration transaction. That
said, we do not need this to be available at launch. It is just the reason why we will want to represent the artifact
schemas in LakeFs.

For the actual storing of artifact type information, we could use protobuf for the schema itself. This would then encode
what will actually be stored in the artifact's object. We can attach metadata in individual fields as needed to define
things like "index this field". We can also use `extend google.protobuf.MessageOptions` to add functionality into the
specific type information. I'd suggest we make the contents of the objects stored in types/<artifact type name> a valid
protobuf definition. We'll have to decide how we load this, though, because we may want to automatically import the
extensions to message metadata and other basic types. We can store fields like viewer endpoints and actions as metadata
which is defined in our extensions to message options such that each artifact type is able to specify those.

As far as specific fields needed in an artifact definition, I think we would need the following:
1. Schema (taken care of by protobuf)
2. Single Field Indexes (add a custom FieldOption which allows specifying an index on a field)
3. Compound Indexes (add a MessageOption which allows creation of one or more compound indexes)
4. Actions (add a MessageOption for defining a dictionary of actions to an artifact)
5. Viewer (add a MessageOption for defining the default viewer endpoint)
6. Custom Instruction (add a MessageOption for defining instructions for LLMs)

