#!/bin/bash
set -eux

# --- CONFIGURATION ---
export PROJECT_ID="your-project-id"
export REGION="us-central1"
export VPC_NAME="lakefs-vpc"
export SUBNET_NAME="lakefs-subnet-${REGION}"
export PROXY_SUBNET_NAME="lakefs-proxy-subnet-${REGION}"

export DB_INSTANCE_NAME="lakefs-db"
export LB_PREFIX="lakefs-lb"
export TEMPLATE_NAME="lakefs-template-v1"
export MIG_NAME="lakefs-mig-regional"
export LAKEFS_VERSION="1.76.0"
export IMAGE_NAME="lakefs-v${LAKEFS_VERSION//./-}-image"
export BUCKET_NAME="lakefs-data-${PROJECT_ID}"
export GC_JOB_NAME="lakefs-gc-daily"

# Service Accounts
export SA_VM_NAME="lakefs-sa"
export SA_SIGNER_NAME="lakefs-signer-sa"
export GC_SA_NAME="lakefs-gc-sa"
export SCHEDULER_SA_NAME="lakefs-scheduler-sa"

# --- SAFETY CHECK ---
echo "========================================================"
echo "   WARNING: TEARDOWN INITIATED FOR PROJECT $PROJECT_ID"
echo "========================================================"
echo "This will PERMANENTLY DELETE:"
echo "  - Cloud SQL Instance ($DB_INSTANCE_NAME)"
echo "  - GCS Bucket (gs://$BUCKET_NAME)"
echo "  - All LakeFS Compute Resources (MIG, LBs, Images)"
echo "  - Networking (VPC, NAT, Subnets, Firewall Rules)"
echo "  - Secrets (DB Password, Signer Key, Auth Keys)"
echo "========================================================"
read -p "Are you sure you want to proceed? (y/N): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo ">>> Aborting."
    exit 1
fi

delete_resource() {
    local TYPE=$1
    local NAME=$2
    local FLAGS=$3
    echo ">>> Deleting $TYPE: $NAME..."
    $TYPE delete "$NAME" $FLAGS --project="$PROJECT_ID" --quiet || echo "    (Skipped or already deleted)"
}

remove_binding() {
    local MEMBER=$1
    local ROLE=$2
    echo ">>> Removing IAM binding: $MEMBER ($ROLE)..."
    gcloud projects remove-iam-policy-binding "$PROJECT_ID" \
        --member="$MEMBER" \
        --role="$ROLE" \
        --condition=None \
        --quiet || echo "    (Binding already removed)"
}

echo ">>> Starting Teardown..."

# 1. Delete Cloud Scheduler
delete_resource "gcloud scheduler jobs" "$GC_JOB_NAME" "--location=$REGION"

# 2. Delete Load Balancer Components (Reverse Order)
delete_resource "gcloud compute forwarding-rules" "${LB_PREFIX}-forwarding-rule" "--region=$REGION"
delete_resource "gcloud compute target-http-proxies" "${LB_PREFIX}-http-proxy" "--region=$REGION"
delete_resource "gcloud compute url-maps" "${LB_PREFIX}-url-map" "--region=$REGION"
delete_resource "gcloud compute backend-services" "${LB_PREFIX}-backend" "--region=$REGION"

# 3. Delete Managed Instance Group (MIG)
delete_resource "gcloud compute instance-groups managed" "$MIG_NAME" "--region=$REGION"

# 4. Delete Dependencies (Health Check & Template)
delete_resource "gcloud compute health-checks" "lakefs-health-check" "--region=$REGION"
delete_resource "gcloud compute instance-templates" "$TEMPLATE_NAME" "--region=$REGION"

# 5. Delete Firewall Rules
delete_resource "gcloud compute firewall-rules" "allow-proxy-to-mig" ""
delete_resource "gcloud compute firewall-rules" "allow-ssh-ingress-from-iap" ""

# 6. Delete Images & Builder
delete_resource "gcloud compute images" "$IMAGE_NAME" ""
delete_resource "gcloud compute instances" "lakefs-builder-temp" "--zone=${REGION}-a"

# 7. Delete Data & Storage
echo ">>> Deleting GCS Bucket: gs://${BUCKET_NAME}..."
gcloud storage rm -r "gs://${BUCKET_NAME}" --project="$PROJECT_ID" --quiet || echo "    (Bucket already gone)"

delete_resource "gcloud sql instances" "$DB_INSTANCE_NAME" ""

echo ">>> Waiting 30s for SQL backend cleanup to release VPC resources..."
sleep 30

# 8. Delete Secrets
delete_resource "gcloud secrets" "lakefs-signer-key" ""
delete_resource "gcloud secrets" "lakefs-db-password" ""
delete_resource "gcloud secrets" "lakefs-secret-key" ""
delete_resource "gcloud secrets" "lakefs-secret-access-key" ""

# 9. Remove Project IAM Bindings
remove_binding "serviceAccount:${SA_VM_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" "roles/secretmanager.secretAccessor"
remove_binding "serviceAccount:${GC_SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" "roles/dataproc.worker"
remove_binding "serviceAccount:${SCHEDULER_SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" "roles/dataproc.editor"

# 10. Delete Identity (Service Accounts)
delete_resource "gcloud iam service-accounts" "${SA_VM_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" ""
delete_resource "gcloud iam service-accounts" "${SA_SIGNER_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" ""
delete_resource "gcloud iam service-accounts" "${GC_SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" ""
delete_resource "gcloud iam service-accounts" "${SCHEDULER_SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" ""

# 11. Delete Networking
delete_resource "gcloud compute routers nats" "lakefs-nat" "--router=lakefs-router --region=$REGION"
delete_resource "gcloud compute routers" "lakefs-router" "--region=$REGION"

echo ">>> Removing VPC Peering to Service Networking..."
MAX_RETRIES=20
COUNT=0
while [ $COUNT -lt $MAX_RETRIES ]; do
    if gcloud services vpc-peerings delete \
        --service=servicenetworking.googleapis.com \
        --network="$VPC_NAME" \
        --project="$PROJECT_ID" --quiet; then
        echo "    Peering deleted successfully."
        break
    fi
    echo "    Peering deletion failed (likely dependent resources). Retrying in 15s... ($COUNT/$MAX_RETRIES)"
    sleep 15
    COUNT=$((COUNT+1))
done

delete_resource "gcloud compute addresses" "google-managed-services-$VPC_NAME" "--global"

echo ">>> Waiting 5s for resources to detach from subnets..."
sleep 5
delete_resource "gcloud compute networks subnets" "$PROXY_SUBNET_NAME" "--region=$REGION"
delete_resource "gcloud compute networks subnets" "$SUBNET_NAME" "--region=$REGION"
delete_resource "gcloud compute networks" "$VPC_NAME" ""

echo "----------------------------------------------------"
echo ">>> Teardown Complete."
echo "----------------------------------------------------"
