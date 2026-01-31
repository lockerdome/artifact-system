#!/bin/bash

# --- CONFIGURATION (Must match provision-idempotent.sh) ---
export PROJECT_ID="your-project-id"
export REGION="us-central1"
export VPC_NAME="lakefs-vpc"
# Updated Subnet Names to match your latest script
export SUBNET_NAME="lakefs-subnet-${REGION}"
export PROXY_SUBNET_NAME="lakefs-proxy-subnet-${REGION}"

export DB_INSTANCE_NAME="lakefs-db"
export LB_PREFIX="lakefs-lb"
export TEMPLATE_NAME="lakefs-template-v1"
export MIG_NAME="lakefs-mig-regional"
export LAKEFS_VERSION="1.76.0"
# Image name logic replicated from provision script
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
echo "  - Networking (VPC, NAT, Subnets: $SUBNET_NAME)"
echo "========================================================"
read -p "Are you sure you want to proceed? (y/N): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo ">>> Aborting."
    exit 1
fi

gcloud config set project "$PROJECT_ID"

# Helper function to delete only if exists to avoid noise
delete_resource() {
    local TYPE=$1
    local NAME=$2
    local FLAGS=$3
    echo ">>> Deleting $TYPE: $NAME..."
    $TYPE delete "$NAME" $FLAGS --quiet || echo "    (Skipped or already deleted)"
}

echo ">>> Starting Teardown..."

# 1. Delete Cloud Scheduler
delete_resource "gcloud scheduler jobs" "$GC_JOB_NAME" "--location=$REGION"

# 2. Delete Load Balancer Components (Reverse Order)
delete_resource "gcloud compute forwarding-rules" "${LB_PREFIX}-forwarding-rule" "--region=$REGION"
delete_resource "gcloud compute target-http-proxies" "${LB_PREFIX}-http-proxy" "--region=$REGION"
delete_resource "gcloud compute url-maps" "${LB_PREFIX}-url-map" "--region=$REGION"
delete_resource "gcloud compute backend-services" "${LB_PREFIX}-backend" "--region=$REGION"
delete_resource "gcloud compute health-checks" "lakefs-health-check" "--region=$REGION"
delete_resource "gcloud compute firewall-rules" "allow-proxy-to-mig" ""

# 3. Delete Compute Resources
# We delete the MIG first to stop instances.
delete_resource "gcloud compute instance-groups managed" "$MIG_NAME" "--region=$REGION"
delete_resource "gcloud compute instance-templates" "$TEMPLATE_NAME" "--region=$REGION"
delete_resource "gcloud compute images" "$IMAGE_NAME" ""

# Check for any leftover Builder VM
delete_resource "gcloud compute instances" "lakefs-builder-temp" "--zone=${REGION}-a"

# 4. Delete Data & Storage
# WARNING: Recursive delete on bucket
echo ">>> Deleting GCS Bucket: gs://${BUCKET_NAME}..."
gcloud storage rm -r "gs://${BUCKET_NAME}" --quiet || echo "    (Bucket already gone)"

delete_resource "gcloud sql instances" "$DB_INSTANCE_NAME" ""

# 5. Delete Secrets
delete_resource "gcloud secrets" "lakefs-signer-key" ""
delete_resource "gcloud secrets" "lakefs-db-password" ""

# 6. Delete Identity (Service Accounts)
delete_resource "gcloud iam service-accounts" "${SA_VM_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" ""
delete_resource "gcloud iam service-accounts" "${SA_SIGNER_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" ""
delete_resource "gcloud iam service-accounts" "${GC_SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" ""
delete_resource "gcloud iam service-accounts" "${SCHEDULER_SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com" ""

# 7. Delete Networking
# A. Cloud NAT & Router
delete_resource "gcloud compute routers nats" "lakefs-nat" "--router=lakefs-router --region=$REGION"
delete_resource "gcloud compute routers" "lakefs-router" "--region=$REGION"

# B. Private Service Access (Peering)
echo ">>> Remove VPC Peering to Service Networking..."
gcloud services vpc-peerings delete \
    --service=servicenetworking.googleapis.com \
    --network="$VPC_NAME" \
    --project="$PROJECT_ID" --quiet || echo "    (Peering already removed)"

# C. The Reserved IP Range for PSA
delete_resource "gcloud compute addresses" "google-managed-services-$VPC_NAME" "--global"

# D. Subnets
# Wait briefly for dependencies to detach
echo ">>> Waiting 10s for resources to detach from subnets..."
sleep 10
delete_resource "gcloud compute networks subnets" "$PROXY_SUBNET_NAME" "--region=$REGION"
delete_resource "gcloud compute networks subnets" "$SUBNET_NAME" "--region=$REGION"

# E. VPC
delete_resource "gcloud compute networks" "$VPC_NAME" ""

echo "----------------------------------------------------"
echo ">>> Teardown Complete."
echo "----------------------------------------------------"
