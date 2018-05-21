node {
    stage("Checkout") {
        git url: 'https://github.com/c0r73x/lsext.git'
        sh 'git clean -fdx; sleep 4;'
    }
    stage("Build") {
        sh "make"
    }
}
