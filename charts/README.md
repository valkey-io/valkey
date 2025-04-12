This is a Helm Chart templates to reproduce valkey crash.
===
[issue-1883](https://github.com/valkey-io/valkey/issues/1883#issuecomment-2787024221)

Dependencies
---
- Helm Chart CLI
- Kubernetes CLI
- EKS
- [Cert manager](https://cert-manager.io/docs/installation/helm/#installing-cert-manager). To Install:
```shell
helm install cert-manager jetstack/cert-manager --namespace cert-manager --create-namespace --version v1.17.0 --set crds.enabled=true
```

Additional info
---
I wasn't able to reproduce the issue in all EC2 instance types. I.e. crash was reproduced on nodes **r6g.2xlarge**,
but never happened on nodes **r7i.2xlarge**.

Steps to reproduce
---
1. Go to AWS console and create node group in EKS with labels ```"app.kubernetes.io/name"=<valkey-cluster-name>```
2. Create namespace **valkey** in K8s:
    ```shell
    kubectl create namespace valkey
    ```
3. Using Helm Chart CLI start new valkey cluster (with the same name as in nodes label on step 1.).
    ```shell
    helm install <valkey-cluster-name> ./charts/valkey-cluster-example --namespace valkey
    ```
    By default, the image [valkey/valkey:8.1.0-alpine3.21](https://hub.docker.com/layers/valkey/valkey/8.1.0-alpine3.21/images/sha256-4a817dbdbb48463d84600067d44f689ec65d3ffce4474bafad10d25f350fced6)
    is used. You can provide you own image if you want:  
    ```shell
    helm install <valkey-cluster-name> ./charts/valkey-cluster-example --namespace valkey --set image=<docker-image>
    ```
4. Wait for when all pods are running.
5. Start traffic generator while produce a ping commands to your cluster:
    ```shell
    helm install ycsb ./charts/ycsb --namespace valkey --set clusterName=<valkey-cluster-name>
    ```
6. Check logs in cluster to see the crash logs.
   ```shell
   18:M 10 Apr 2025 18:13:53.586 # Illegal state for flags (0) of TLS connection (with state 2) which is 6 ms old: (addr=10.66.30.114:34228 laddr=10.92.190.56:17379)
   
   
   === VALKEY BUG REPORT START: Cut & paste starting from here ===
   18:M 10 Apr 2025 18:13:53.586 # === ASSERTION FAILED ===
   18:M 10 Apr 2025 18:13:53.586 # ==> tls.c:607 '0' is not true
   ```

How to uninstall
---
1. Traffic generator:
    ```shell
    helm uninstall ycsb --namespace valkey
    ```
2. Cluster:
    ```shell
    helm uninstall <valkey-cluster-name> --namespace valkey
    ```
